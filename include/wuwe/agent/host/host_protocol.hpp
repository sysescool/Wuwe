#ifndef WUWE_AGENT_HOST_HOST_PROTOCOL_HPP
#define WUWE_AGENT_HOST_HOST_PROTOCOL_HPP

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/core/metadata.hpp>

namespace wuwe::agent::host {

inline constexpr std::string_view default_protocol_version = "2026-07-01";
inline constexpr std::array<std::string_view, 1> supported_protocol_versions {
  default_protocol_version,
};

enum class host_operation {
  create_run,
  get_run,
  cancel_run,
  resolve_approval,
  resume_run,
  list_events,
};

enum class host_error_code {
  invalid_request,
  unsupported_protocol_version,
  unauthorized,
  forbidden,
  not_found,
  conflict,
  unavailable,
  internal,
};

[[nodiscard]] inline std::string to_string(host_operation operation) {
  switch (operation) {
    case host_operation::create_run:
      return "create_run";
    case host_operation::get_run:
      return "get_run";
    case host_operation::cancel_run:
      return "cancel_run";
    case host_operation::resolve_approval:
      return "resolve_approval";
    case host_operation::resume_run:
      return "resume_run";
    case host_operation::list_events:
      return "list_events";
  }
  return "get_run";
}

[[nodiscard]] inline host_operation host_operation_from_string(std::string_view value) {
  if (value == "create_run")
    return host_operation::create_run;
  if (value == "get_run")
    return host_operation::get_run;
  if (value == "cancel_run")
    return host_operation::cancel_run;
  if (value == "resolve_approval")
    return host_operation::resolve_approval;
  if (value == "resume_run")
    return host_operation::resume_run;
  if (value == "list_events")
    return host_operation::list_events;
  throw std::invalid_argument("unsupported agent host operation");
}

[[nodiscard]] inline std::string to_string(host_error_code code) {
  switch (code) {
    case host_error_code::invalid_request:
      return "invalid_request";
    case host_error_code::unsupported_protocol_version:
      return "unsupported_protocol_version";
    case host_error_code::unauthorized:
      return "unauthorized";
    case host_error_code::forbidden:
      return "forbidden";
    case host_error_code::not_found:
      return "not_found";
    case host_error_code::conflict:
      return "conflict";
    case host_error_code::unavailable:
      return "unavailable";
    case host_error_code::internal:
      return "internal";
  }
  return "internal";
}

[[nodiscard]] inline host_error_code host_error_code_from_string(std::string_view value) {
  if (value == "invalid_request")
    return host_error_code::invalid_request;
  if (value == "unsupported_protocol_version") {
    return host_error_code::unsupported_protocol_version;
  }
  if (value == "unauthorized")
    return host_error_code::unauthorized;
  if (value == "forbidden")
    return host_error_code::forbidden;
  if (value == "not_found")
    return host_error_code::not_found;
  if (value == "conflict")
    return host_error_code::conflict;
  if (value == "unavailable")
    return host_error_code::unavailable;
  if (value == "internal")
    return host_error_code::internal;
  throw std::invalid_argument("unsupported agent host error code");
}

[[nodiscard]] inline bool supports_protocol_version(std::string_view version) noexcept {
  return std::find(supported_protocol_versions.begin(),
           supported_protocol_versions.end(),
           version) != supported_protocol_versions.end();
}

[[nodiscard]] inline std::optional<std::string> negotiate_protocol_version(
  const std::vector<std::string>& offered_versions) {
  for (const auto supported : supported_protocol_versions) {
    if (std::find(offered_versions.begin(), offered_versions.end(), supported) !=
        offered_versions.end()) {
      return std::string(supported);
    }
  }
  return std::nullopt;
}

[[nodiscard]] inline bool mutating(host_operation operation) noexcept {
  return operation != host_operation::get_run && operation != host_operation::list_events;
}

struct host_call_context {
  std::string protocol_version { default_protocol_version };
  std::string request_id;
  std::string idempotency_key;
  std::map<std::string, std::string> metadata;
};

struct host_request_envelope {
  host_call_context call;
  host_operation operation { host_operation::get_run };
  nlohmann::json body = nlohmann::json::object();
};

struct host_error {
  host_error_code code { host_error_code::internal };
  std::string message;
  bool retryable { false };
  nlohmann::json details = nlohmann::json::object();
  std::map<std::string, std::string> metadata;
};

struct host_response_envelope {
  std::string protocol_version { default_protocol_version };
  std::string request_id;
  host_operation operation { host_operation::get_run };
  nlohmann::json body = nlohmann::json::object();
  std::optional<host_error> error;

  [[nodiscard]] explicit operator bool() const noexcept {
    return !error.has_value();
  }
};

inline void validate_host_request(const host_request_envelope& request) {
  if (!supports_protocol_version(request.call.protocol_version)) {
    throw std::invalid_argument("unsupported agent host protocol version");
  }
  if (request.call.request_id.empty()) {
    throw std::invalid_argument("agent host request requires a request_id");
  }
  if (mutating(request.operation) && request.call.idempotency_key.empty()) {
    throw std::invalid_argument("mutating agent host request requires an idempotency_key");
  }
  if (!request.body.is_object()) {
    throw std::invalid_argument("agent host request body must be an object");
  }
}

[[nodiscard]] inline nlohmann::json host_request_to_json(const host_request_envelope& request) {
  validate_host_request(request);
  return {
    { "protocolVersion", request.call.protocol_version },
    { "requestId", request.call.request_id },
    { "idempotencyKey", request.call.idempotency_key },
    { "operation", to_string(request.operation) },
    { "metadata", request.call.metadata },
    { "body", request.body },
  };
}

[[nodiscard]] inline host_request_envelope host_request_from_json(const nlohmann::json& value) {
  if (!value.is_object()) {
    throw std::invalid_argument("agent host request must be an object");
  }
  const auto version = value.find("protocolVersion");
  if (version == value.end() || !version->is_string() ||
      version->get_ref<const std::string&>().empty()) {
    throw std::invalid_argument("agent host request requires a protocolVersion");
  }
  host_request_envelope request {
    .call = {
      .protocol_version = version->get<std::string>(),
      .request_id = value.value("requestId", std::string {}),
      .idempotency_key = value.value("idempotencyKey", std::string {}),
      .metadata = value.value(
        "metadata", std::map<std::string, std::string> {}),
    },
    .operation = host_operation_from_string(
      value.value("operation", std::string {})),
    .body = value.value("body", nlohmann::json::object()),
  };
  validate_host_request(request);
  return request;
}

[[nodiscard]] inline nlohmann::json host_error_to_json(const host_error& error) {
  std::map<std::string, std::string> metadata;
  for (const auto& [key, value] : error.metadata) {
    if (!core::sensitive_metadata_key(key)) {
      metadata.emplace(key, value);
    }
  }
  return {
    { "code", to_string(error.code) },
    { "message", error.message },
    { "retryable", error.retryable },
    { "details", error.details },
    { "metadata", std::move(metadata) },
  };
}

[[nodiscard]] inline host_error host_error_from_json(const nlohmann::json& value) {
  if (!value.is_object()) {
    throw std::invalid_argument("agent host error must be an object");
  }
  return {
    .code = host_error_code_from_string(value.value("code", std::string {})),
    .message = value.value("message", std::string {}),
    .retryable = value.value("retryable", false),
    .details = value.value("details", nlohmann::json::object()),
    .metadata = value.value("metadata", std::map<std::string, std::string> {}),
  };
}

[[nodiscard]] inline nlohmann::json host_response_to_json(const host_response_envelope& response) {
  return {
    { "protocolVersion", response.protocol_version },
    { "requestId", response.request_id },
    { "operation", to_string(response.operation) },
    { "ok", !response.error.has_value() },
    { "body", response.body },
    { "error", response.error ? host_error_to_json(*response.error) : nlohmann::json(nullptr) },
  };
}

[[nodiscard]] inline host_response_envelope host_response_from_json(const nlohmann::json& value) {
  if (!value.is_object()) {
    throw std::invalid_argument("agent host response must be an object");
  }
  const auto version = value.find("protocolVersion");
  if (version == value.end() || !version->is_string() ||
      version->get_ref<const std::string&>().empty()) {
    throw std::invalid_argument("agent host response requires a protocolVersion");
  }
  host_response_envelope response {
    .protocol_version = version->get<std::string>(),
    .request_id = value.value("requestId", std::string {}),
    .operation = host_operation_from_string(value.value("operation", std::string {})),
    .body = value.value("body", nlohmann::json::object()),
  };
  if (!supports_protocol_version(response.protocol_version)) {
    throw std::invalid_argument("unsupported agent host response version");
  }
  if (!response.body.is_object()) {
    throw std::invalid_argument("invalid agent host response envelope");
  }
  if (value.contains("error") && !value.at("error").is_null()) {
    response.error = host_error_from_json(value.at("error"));
  }
  const auto ok = value.value("ok", !response.error.has_value());
  if (ok == response.error.has_value()) {
    throw std::invalid_argument("inconsistent agent host response status");
  }
  if (response.request_id.empty() && !response.error) {
    throw std::invalid_argument("successful agent host response requires a requestId");
  }
  return response;
}

} // namespace wuwe::agent::host

#endif // WUWE_AGENT_HOST_HOST_PROTOCOL_HPP
