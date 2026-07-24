#ifndef WUWE_AGENT_A2A_A2A_SERVICE_HPP
#define WUWE_AGENT_A2A_A2A_SERVICE_HPP

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <wuwe/agent/a2a/a2a_transport.hpp>

namespace wuwe::agent::a2a {

class task_handler {
public:
  virtual ~task_handler() = default;
  virtual result<task> send(
    const send_message_params& params,
    std::stop_token stop_token) = 0;
  virtual result<task> get(const task_query_params& params) = 0;
  virtual result<task> cancel(const task_id_params& params) = 0;
};

class function_task_handler final : public task_handler {
public:
  using send_callback =
    std::function<result<task>(const send_message_params&, std::stop_token)>;
  using get_callback = std::function<result<task>(const task_query_params&)>;
  using cancel_callback = std::function<result<task>(const task_id_params&)>;

  function_task_handler(
    send_callback send,
    get_callback get,
    cancel_callback cancel)
      : send_(std::move(send)), get_(std::move(get)), cancel_(std::move(cancel)) {
    if (!send_ || !get_ || !cancel_) {
      throw std::invalid_argument(
        "function_task_handler requires send, get, and cancel callbacks");
    }
  }

  result<task> send(
    const send_message_params& params,
    std::stop_token stop_token) override {
    return send_(params, stop_token);
  }

  result<task> get(const task_query_params& params) override {
    return get_(params);
  }

  result<task> cancel(const task_id_params& params) override {
    return cancel_(params);
  }

private:
  send_callback send_;
  get_callback get_;
  cancel_callback cancel_;
};

class service {
public:
  service(agent_card card, std::shared_ptr<task_handler> handler)
      : card_(std::move(card)), handler_(std::move(handler)) {
    if (card_.name.empty() || card_.url.empty()) {
      throw std::invalid_argument("A2A service Agent Card requires name and url");
    }
    if (card_.capabilities.streaming || card_.capabilities.push_notifications) {
      throw std::invalid_argument(
        "the synchronous A2A service cannot advertise streaming or push notifications");
    }
    if (!handler_) {
      throw std::invalid_argument("A2A service requires a task handler");
    }
  }

  [[nodiscard]] const agent_card& card() const noexcept {
    return card_;
  }

  [[nodiscard]] nlohmann::json handle_jsonrpc(
    const nlohmann::json& request,
    std::stop_token stop_token = {}) const {
    if (!request.is_object()) {
      return error_response(nullptr, {
        .code = error_code::invalid_request,
        .message = "invalid JSON-RPC request",
      });
    }
    const auto notification = !request.contains("id");
    const auto id = notification ? nlohmann::json(nullptr) : request.at("id");
    const auto respond = [&](nlohmann::json response) {
      return notification ? nlohmann::json() : std::move(response);
    };
    try {
      if (!request.contains("jsonrpc") || !request.at("jsonrpc").is_string() ||
          request.at("jsonrpc") != "2.0" || !request.contains("method") ||
          !request.at("method").is_string()) {
        return respond(error_response(id, {
          .code = error_code::invalid_request,
          .message = "invalid JSON-RPC request",
        }));
      }
      if (!notification && !id.is_null() && !id.is_string() &&
          !id.is_number_integer() && !id.is_number_unsigned()) {
        return error_response(nullptr, {
          .code = error_code::invalid_request,
          .message = "JSON-RPC id must be a string, integer, or null",
        });
      }
      const auto method = request.at("method").get<std::string>();
      const auto params = request.value("params", nlohmann::json::object());
      if (!params.is_object()) {
        return respond(error_response(id, {
          .code = error_code::invalid_params,
          .message = "JSON-RPC params must be an object",
        }));
      }
      result<task> handled;
      if (method == "message/send") {
        auto send_params = send_params_from_json(params);
        if (!send_params.configuration.accepted_output_modes.empty() &&
            std::none_of(
              send_params.configuration.accepted_output_modes.begin(),
              send_params.configuration.accepted_output_modes.end(),
              [&](const auto& accepted) {
                return std::find(
                         card_.default_output_modes.begin(),
                         card_.default_output_modes.end(),
                         accepted) != card_.default_output_modes.end();
              })) {
          handled = { .failure = error {
            .code = error_code::content_type_not_supported,
            .message = "the A2A service does not support any accepted output mode",
          } };
        }
        else {
          handled = handler_->send(send_params, stop_token);
        }
      }
      else if (method == "tasks/get") {
        handled = handler_->get(task_query_from_json(params));
      }
      else if (method == "tasks/cancel") {
        handled = handler_->cancel(task_id_from_json(params));
      }
      else {
        return respond(error_response(id, {
          .code = error_code::method_not_found,
          .message = "A2A method not found: " + method,
        }));
      }
      if (!handled) {
        return respond(error_response(id, handled.failure.value_or(error {
          .code = error_code::internal_error,
          .message = "A2A handler returned no result",
        })));
      }
      return respond({
        { "jsonrpc", "2.0" },
        { "id", id },
        { "result", to_json(*handled.value) },
      });
    }
    catch (const std::invalid_argument& ex) {
      return respond(error_response(id, {
        .code = error_code::invalid_params,
        .message = ex.what(),
      }));
    }
    catch (const nlohmann::json::exception& ex) {
      return respond(error_response(id, {
        .code = error_code::invalid_params,
        .message = ex.what(),
      }));
    }
    catch (const std::exception& ex) {
      return respond(error_response(id, {
        .code = error_code::internal_error,
        .message = ex.what(),
      }));
    }
    catch (...) {
      return respond(error_response(id, {
        .code = error_code::internal_error,
        .message = "A2A service failed with an unknown exception",
      }));
    }
  }

private:
  static nlohmann::json error_response(const nlohmann::json& id, const error& failure) {
    nlohmann::json body {
      { "code", static_cast<int>(failure.code) },
      { "message", failure.message },
    };
    if (failure.data) body["data"] = *failure.data;
    return {
      { "jsonrpc", "2.0" },
      { "id", id },
      { "error", std::move(body) },
    };
  }

  static send_message_params send_params_from_json(const nlohmann::json& value) {
    if (!value.contains("message")) {
      throw std::invalid_argument("message/send requires message");
    }
    send_message_params output;
    output.value = message_from_json(value.at("message"));
    const auto configuration = value.value("configuration", nlohmann::json::object());
    output.configuration.accepted_output_modes =
      configuration.value("acceptedOutputModes", std::vector<std::string> {});
    output.configuration.history_length = configuration.value("historyLength", std::size_t {});
    output.configuration.blocking = configuration.value("blocking", true);
    output.metadata = value.value("metadata", nlohmann::json::object());
    return output;
  }

  static task_query_params task_query_from_json(const nlohmann::json& value) {
    task_query_params output {
      .id = value.value("id", ""),
      .history_length = value.value("historyLength", std::size_t {}),
      .metadata = value.value("metadata", nlohmann::json::object()),
    };
    if (output.id.empty()) throw std::invalid_argument("tasks/get requires id");
    return output;
  }

  static task_id_params task_id_from_json(const nlohmann::json& value) {
    task_id_params output {
      .id = value.value("id", ""),
      .metadata = value.value("metadata", nlohmann::json::object()),
    };
    if (output.id.empty()) throw std::invalid_argument("tasks/cancel requires id");
    return output;
  }

  agent_card card_;
  std::shared_ptr<task_handler> handler_;
};

class in_process_transport final : public transport {
public:
  explicit in_process_transport(std::shared_ptr<service> service)
      : service_(std::move(service)) {
    if (!service_) {
      throw std::invalid_argument("in-process A2A transport requires a service");
    }
  }

  rpc_result invoke(
    std::string method,
    nlohmann::json params,
    std::stop_token stop_token = {}) override {
    if (stop_token.stop_requested()) {
      return { .failure = error {
        .code = error_code::transport_error,
        .message = "A2A request cancelled before in-process transport",
      } };
    }
    auto response = service_->handle_jsonrpc({
      { "jsonrpc", "2.0" },
      { "id", ++sequence_ },
      { "method", std::move(method) },
      { "params", std::move(params) },
    }, stop_token);
    if (response.contains("error")) {
      const auto body = response.at("error");
      return { .failure = error {
        .code = static_cast<error_code>(body.value("code", -32603)),
        .message = body.value("message", "A2A service error"),
        .data = body.contains("data")
                  ? std::optional<nlohmann::json>(body.at("data"))
                  : std::nullopt,
      } };
    }
    return { .value = response.value("result", nlohmann::json()) };
  }

  result<agent_card> discover(std::stop_token stop_token = {}) override {
    if (stop_token.stop_requested()) {
      return { .failure = error {
        .code = error_code::transport_error,
        .message = "A2A discovery cancelled",
      } };
    }
    return { .value = service_->card() };
  }

  [[nodiscard]] transport_capabilities capabilities() const noexcept override {
    return {
      .cooperative_cancellation = true,
      .concurrent_invocation = true,
    };
  }

private:
  std::shared_ptr<service> service_;
  std::atomic<std::uint64_t> sequence_ { 0 };
};

} // namespace wuwe::agent::a2a

#endif // WUWE_AGENT_A2A_A2A_SERVICE_HPP
