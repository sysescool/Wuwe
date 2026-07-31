#ifndef WUWE_AGENT_HOST_HOST_DISPATCHER_HPP
#define WUWE_AGENT_HOST_HOST_DISPATCHER_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <wuwe/agent/host/host_codec.hpp>

namespace wuwe::agent::host {

class host_dispatcher {
public:
  explicit host_dispatcher(agent_host_service& service) : service_(service) {
  }

  [[nodiscard]] host_response_envelope dispatch(const host_request_envelope& request) const {
    if (!supports_protocol_version(request.call.protocol_version)) {
      return failure(request,
        host_error_code::unsupported_protocol_version,
        "unsupported agent host protocol version",
        false,
        {
          { "supportedVersions", supported_protocol_versions },
        });
    }
    try {
      validate_host_request(request);
    }
    catch (const std::invalid_argument& ex) {
      return failure(request, host_error_code::invalid_request, ex.what());
    }
    catch (const nlohmann::json::exception&) {
      return failure(request, host_error_code::invalid_request, "malformed agent host request");
    }
    catch (...) {
      return failure(request, host_error_code::internal, "agent host request validation failed");
    }

    switch (request.operation) {
      case host_operation::create_run:
        return dispatch_operation(
          request,
          [&] { return create_run_request_from_json(request.body); },
          [&](const create_run_request& decoded) {
            return service_.create_run(request.call, decoded);
          },
          run_submission_to_json);
      case host_operation::get_run:
        return dispatch_operation(
          request,
          [&] { return get_run_request_from_json(request.body); },
          [&](const get_run_request& decoded) { return service_.get_run(request.call, decoded); },
          run_view_to_json);
      case host_operation::cancel_run:
        return dispatch_operation(
          request,
          [&] { return cancel_run_request_from_json(request.body); },
          [&](const cancel_run_request& decoded) {
            return service_.cancel_run(request.call, decoded);
          },
          run_submission_to_json);
      case host_operation::resolve_approval:
        return dispatch_operation(
          request,
          [&] { return resolve_approval_request_from_json(request.body); },
          [&](const resolve_approval_request& decoded) {
            return service_.resolve_approval(request.call, decoded);
          },
          run_submission_to_json);
      case host_operation::resume_run:
        return dispatch_operation(
          request,
          [&] { return resume_run_request_from_json(request.body); },
          [&](const resume_run_request& decoded) {
            return service_.resume_run(request.call, decoded);
          },
          run_submission_to_json);
      case host_operation::list_events:
        return dispatch_event_page(request);
    }
    return failure(request, host_error_code::internal, "agent host operation failed");
  }

  [[nodiscard]] nlohmann::json dispatch_json(const nlohmann::json& encoded) const {
    std::string request_id;
    host_operation operation { host_operation::get_run };
    try {
      if (encoded.is_object()) {
        request_id = encoded.value("requestId", std::string {});
        const auto version = encoded.find("protocolVersion");
        if (version != encoded.end() && version->is_string() &&
            !supports_protocol_version(version->get<std::string>())) {
          return host_response_to_json(failure(request_id,
            operation,
            host_error_code::unsupported_protocol_version,
            "unsupported agent host protocol version",
            false,
            {
              { "supportedVersions", supported_protocol_versions },
            }));
        }
        if (const auto found = encoded.find("operation");
            found != encoded.end() && found->is_string()) {
          operation = host_operation_from_string(found->get<std::string>());
        }
      }
      return host_response_to_json(dispatch(host_request_from_json(encoded)));
    }
    catch (const std::invalid_argument& ex) {
      return host_response_to_json(
        failure(request_id, operation, host_error_code::invalid_request, ex.what()));
    }
    catch (const nlohmann::json::exception&) {
      return host_response_to_json(failure(
        request_id, operation, host_error_code::invalid_request, "malformed agent host request"));
    }
    catch (...) {
      return host_response_to_json(failure(request_id,
        operation,
        host_error_code::internal,
        "agent host request could not be dispatched"));
    }
  }

private:
  host_response_envelope dispatch_event_page(const host_request_envelope& request) const {
    list_events_request query;
    try {
      query = list_events_request_from_json(request.body);
    }
    catch (const std::invalid_argument& ex) {
      return failure(request, host_error_code::invalid_request, ex.what());
    }
    catch (const nlohmann::json::exception&) {
      return failure(request, host_error_code::invalid_request, "malformed list_events request");
    }
    catch (...) {
      return failure(
        request, host_error_code::internal, "agent host list_events request decoding failed");
    }
    try {
      auto result = service_.list_events(request.call, query);
      if (const auto* page = result.value_if()) {
        if (page->events.size() > query.limit || page->next_sequence < query.after_sequence ||
            (page->events.empty() &&
              (page->next_sequence != query.after_sequence || page->has_more))) {
          return failure(request,
            host_error_code::internal,
            "agent host service returned an invalid event page");
        }
        for (const auto& event : page->events) {
          if (event.run_id != query.run_id || event.sequence <= query.after_sequence) {
            return failure(request,
              host_error_code::internal,
              "agent host service returned events outside the requested cursor");
          }
        }
      }
      return dispatch_result(request, std::move(result), event_page_to_json);
    }
    catch (...) {
      return failure(
        request, host_error_code::internal, "agent host service failed to list events");
    }
  }

  template<typename Decoder, typename Invoker, typename Encoder>
  host_response_envelope dispatch_operation(const host_request_envelope& request, Decoder&& decode,
    Invoker&& invoke, Encoder&& encode) const {
    try {
      auto decoded = std::forward<Decoder>(decode)();
      try {
        auto result = std::forward<Invoker>(invoke)(decoded);
        return dispatch_result(request, std::move(result), std::forward<Encoder>(encode));
      }
      catch (...) {
        return failure(request, host_error_code::internal, "agent host service operation failed");
      }
    }
    catch (const std::invalid_argument& ex) {
      return failure(request, host_error_code::invalid_request, ex.what());
    }
    catch (const nlohmann::json::exception&) {
      return failure(
        request, host_error_code::invalid_request, "malformed agent host operation body");
    }
    catch (...) {
      return failure(
        request, host_error_code::internal, "agent host operation body decoding failed");
    }
  }

  template<typename T, typename Encoder>
  static host_response_envelope dispatch_result(
    const host_request_envelope& request, host_result<T> result, Encoder&& encode) {
    if (const auto* error = result.error_if()) {
      if (error->code == host_error_code::internal) {
        auto sanitized = *error;
        sanitized.message = "agent host service operation failed";
        sanitized.details = nlohmann::json::object();
        sanitized.metadata.clear();
        return failure(request, std::move(sanitized));
      }
      return failure(request, *error);
    }
    const auto* value = result.value_if();
    return {
      .protocol_version = std::string(default_protocol_version),
      .request_id = request.call.request_id,
      .operation = request.operation,
      .body = std::forward<Encoder>(encode)(*value),
    };
  }

  static host_response_envelope failure(const host_request_envelope& request, host_error error) {
    return {
      .protocol_version = std::string(default_protocol_version),
      .request_id = request.call.request_id,
      .operation = request.operation,
      .error = std::move(error),
    };
  }

  static host_response_envelope failure(const host_request_envelope& request, host_error_code code,
    std::string message, bool retryable = false,
    nlohmann::json details = nlohmann::json::object()) {
    return failure(request,
      { .code = code,
        .message = std::move(message),
        .retryable = retryable,
        .details = std::move(details) });
  }

  static host_response_envelope failure(std::string request_id, host_operation operation,
    host_error_code code, std::string message, bool retryable = false,
    nlohmann::json details = nlohmann::json::object()) {
    return {
      .protocol_version = std::string(default_protocol_version),
      .request_id = std::move(request_id),
      .operation = operation,
      .error =
        host_error {
          .code = code,
          .message = std::move(message),
          .retryable = retryable,
          .details = std::move(details),
        },
    };
  }

  agent_host_service& service_;
};

} // namespace wuwe::agent::host

#endif // WUWE_AGENT_HOST_HOST_DISPATCHER_HPP
