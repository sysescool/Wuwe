#ifndef WUWE_AGENT_TOOLS_TOOL_CONTRACT_HPP
#define WUWE_AGENT_TOOLS_TOOL_CONTRACT_HPP

#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/capability/capability.hpp>
#include <wuwe/agent/core/execution_context.hpp>
#include <wuwe/agent/llm/llm_types.h>
#include <wuwe/agent/llm/tool_output_projection_types.hpp>

namespace wuwe::agent::tools {

struct tool_descriptor;
inline void validate_tool_descriptor(const tool_descriptor& value);

enum class tool_side_effect {
  none,
  read,
  write,
  destructive,
};

enum class tool_idempotency {
  unknown,
  idempotent,
  idempotent_with_key,
  non_idempotent,
};

enum class tool_approval_mode {
  never,
  policy,
  always,
};

enum class tool_error_category {
  none,
  invalid_input,
  not_found,
  permission_denied,
  conflict,
  rate_limited,
  timeout,
  cancelled,
  unavailable,
  internal,
};

enum class tool_output_validation_mode {
  disabled,
  warn,
  strict,
};

struct tool_retry_policy {
  std::size_t max_attempts { 1 };
  std::chrono::milliseconds initial_backoff { 100 };
  std::chrono::milliseconds maximum_backoff { 5000 };
  double backoff_multiplier { 2.0 };
  double jitter_ratio { 0.1 };
  std::set<tool_error_category> retryable_categories;
};

struct tool_resource_version_policy {
  bool require_expected_version { false };
  std::string argument_json_pointer { "/resource_version" };
  std::string outcome_json_pointer { "/resource_version" };
};

struct tool_compensation_policy {
  bool enabled { false };
  std::chrono::milliseconds timeout { 0 };
  std::size_t max_attempts { 1 };
};

struct tool_heartbeat_policy {
  std::chrono::milliseconds timeout { 0 };
  std::chrono::milliseconds minimum_interval { 250 };
};

struct tool_heartbeat {
  std::string message;
  std::optional<double> progress;
  std::map<std::string, std::string> metadata;
  std::chrono::steady_clock::time_point timestamp { std::chrono::steady_clock::now() };
};

// Provider capabilities describe which parts of tool_invocation are honored,
// rather than merely which overloads happen to exist. Wrapper providers must
// forward these values per tool so safety decisions survive composition.
struct tool_provider_capabilities {
  bool invocation_context { false };
  bool idempotency_key { false };
  bool heartbeat { false };
  bool compensation { false };
  bool declaration_valid { true };
};

[[nodiscard]] inline bool valid_tool_provider_capabilities(
  const tool_provider_capabilities& value) noexcept {
  return value.declaration_valid &&
         (value.invocation_context || (!value.idempotency_key && !value.heartbeat));
}

[[nodiscard]] inline std::string to_string(tool_side_effect value) {
  switch (value) {
    case tool_side_effect::none:
      return "none";
    case tool_side_effect::read:
      return "read";
    case tool_side_effect::write:
      return "write";
    case tool_side_effect::destructive:
      return "destructive";
  }
  return "none";
}

[[nodiscard]] inline std::string to_string(tool_idempotency value) {
  switch (value) {
    case tool_idempotency::unknown:
      return "unknown";
    case tool_idempotency::idempotent:
      return "idempotent";
    case tool_idempotency::idempotent_with_key:
      return "idempotent_with_key";
    case tool_idempotency::non_idempotent:
      return "non_idempotent";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(tool_approval_mode value) {
  switch (value) {
    case tool_approval_mode::never:
      return "never";
    case tool_approval_mode::policy:
      return "policy";
    case tool_approval_mode::always:
      return "always";
  }
  return "never";
}

[[nodiscard]] inline std::string to_string(tool_error_category value) {
  switch (value) {
    case tool_error_category::none:
      return "none";
    case tool_error_category::invalid_input:
      return "invalid_input";
    case tool_error_category::not_found:
      return "not_found";
    case tool_error_category::permission_denied:
      return "permission_denied";
    case tool_error_category::conflict:
      return "conflict";
    case tool_error_category::rate_limited:
      return "rate_limited";
    case tool_error_category::timeout:
      return "timeout";
    case tool_error_category::cancelled:
      return "cancelled";
    case tool_error_category::unavailable:
      return "unavailable";
    case tool_error_category::internal:
      return "internal";
  }
  return "internal";
}

[[nodiscard]] inline std::string to_string(tool_output_validation_mode value) {
  switch (value) {
    case tool_output_validation_mode::disabled:
      return "disabled";
    case tool_output_validation_mode::warn:
      return "warn";
    case tool_output_validation_mode::strict:
      return "strict";
  }
  return "strict";
}

using tool_capability_requirement = capability::capability_requirement;

struct tool_descriptor {
  std::string name;
  std::string version { "1" };
  std::string description;
  nlohmann::json input_schema = nlohmann::json::object();
  nlohmann::json output_schema = nlohmann::json::object();
  tool_side_effect side_effect { tool_side_effect::none };
  tool_idempotency idempotency { tool_idempotency::unknown };
  tool_approval_mode approval { tool_approval_mode::never };
  std::chrono::milliseconds timeout { 0 };
  tool_output_validation_mode output_validation { tool_output_validation_mode::strict };
  tool_retry_policy retry;
  tool_resource_version_policy resource_version;
  tool_compensation_policy compensation;
  tool_heartbeat_policy heartbeat;
  std::vector<tool_capability_requirement> capabilities;
  std::map<std::string, std::string> metadata;
  agent::llm::tool_output_projection_constraints model_output_projection;

  [[nodiscard]] llm_tool model_tool() const {
    validate_tool_descriptor(*this);
    return {
      .name = name,
      .description = description,
      .parameters_json_schema = input_schema.dump(),
    };
  }
};

inline void validate_tool_descriptor(const tool_descriptor& value) {
  if (value.name.empty()) {
    throw std::invalid_argument("tool descriptor requires a name");
  }
  if (value.version.empty()) {
    throw std::invalid_argument("tool descriptor requires a version");
  }
  if (!value.input_schema.is_object()) {
    throw std::invalid_argument("tool input schema must be a JSON object");
  }
  if (!value.output_schema.is_object()) {
    throw std::invalid_argument("tool output schema must be a JSON object");
  }
  if (value.timeout < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("tool timeout must not be negative");
  }
  if (value.retry.max_attempts == 0) {
    throw std::invalid_argument("tool retry policy requires at least one attempt");
  }
  if (value.retry.initial_backoff < std::chrono::milliseconds::zero() ||
      value.retry.maximum_backoff < std::chrono::milliseconds::zero() ||
      value.retry.initial_backoff > value.retry.maximum_backoff) {
    throw std::invalid_argument("tool retry backoff range is invalid");
  }
  if (!std::isfinite(value.retry.backoff_multiplier) || !std::isfinite(value.retry.jitter_ratio) ||
      value.retry.backoff_multiplier < 1.0 || value.retry.jitter_ratio < 0.0 ||
      value.retry.jitter_ratio > 1.0) {
    throw std::invalid_argument("tool retry multiplier or jitter is invalid");
  }
  if (value.retry.max_attempts > 1 &&
      (value.idempotency == tool_idempotency::unknown ||
        value.idempotency == tool_idempotency::non_idempotent) &&
      !value.compensation.enabled) {
    throw std::invalid_argument("non-idempotent tool retries require compensation");
  }
  if (value.resource_version.require_expected_version &&
      value.resource_version.argument_json_pointer.empty()) {
    throw std::invalid_argument("resource version policy requires an argument JSON Pointer");
  }
  for (const auto* pointer : { &value.resource_version.argument_json_pointer,
         &value.resource_version.outcome_json_pointer }) {
    if (!pointer->empty() && pointer->front() != '/') {
      throw std::invalid_argument("resource version paths must be JSON Pointers");
    }
    if (!pointer->empty()) {
      try {
        (void)nlohmann::json::json_pointer(*pointer);
      }
      catch (const nlohmann::json::parse_error&) {
        throw std::invalid_argument("resource version path is not a valid JSON Pointer");
      }
    }
  }
  if (value.compensation.max_attempts == 0 ||
      value.compensation.timeout < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("tool compensation policy is invalid");
  }
  if (value.heartbeat.timeout < std::chrono::milliseconds::zero() ||
      value.heartbeat.minimum_interval < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("tool heartbeat policy is invalid");
  }
  if (value.model_output_projection.max_bytes &&
      *value.model_output_projection.max_bytes <
        agent::llm::minimum_tool_output_projection_max_bytes) {
    throw std::invalid_argument("tool model-output byte limit is below the supported minimum");
  }
  if (value.model_output_projection.max_tokens &&
      *value.model_output_projection.max_tokens <
        agent::llm::minimum_tool_output_projection_max_tokens) {
    throw std::invalid_argument("tool model-output token limit is below the supported minimum");
  }
  std::set<std::string> capability_names;
  for (const auto& requirement : value.capabilities) {
    if (requirement.name.empty()) {
      throw std::invalid_argument("tool capability requirement requires a name");
    }
    if (!capability_names.insert(requirement.name).second) {
      throw std::invalid_argument("duplicate tool capability requirement: " + requirement.name);
    }
  }
}

struct tool_outcome {
  std::string content;
  std::error_code error_code;
  nlohmann::json data;
  tool_error_category error_category { tool_error_category::none };
  bool retryable { false };
  std::optional<std::string> resource_version;
  bool compensation_required { false };
  std::string compensation_token;
  std::vector<std::string> artifacts;
  std::map<std::string, std::string> metadata;

  [[nodiscard]] bool succeeded() const noexcept {
    return !error_code && error_category == tool_error_category::none;
  }
};

struct tool_invocation {
  std::string call_id;
  std::string name;
  std::string arguments_json;
  std::string idempotency_key;
  std::size_t attempt { 1 };
  std::optional<std::string> expected_resource_version;
  tool_descriptor descriptor;
  core::agent_execution_context context;
  std::stop_token stop_token;
  std::function<void(tool_heartbeat)> report_heartbeat;
};

template<typename Provider>
[[nodiscard]] tool_provider_capabilities resolve_tool_provider_capabilities(
  Provider& provider, const std::string& name) {
  constexpr bool invocation_aware = requires(
    Provider & candidate, const tool_invocation& invocation) { candidate.invoke(invocation); };
  constexpr bool compensating =
    requires(Provider & candidate, const tool_invocation& invocation, const tool_outcome& outcome) {
      { candidate.compensate(invocation, outcome) } -> std::convertible_to<tool_outcome>;
    };

  if constexpr (requires {
                  {
                    provider.contract_capabilities(name)
                    } -> std::convertible_to<tool_provider_capabilities>;
                }) {
    auto capabilities =
      static_cast<tool_provider_capabilities>(provider.contract_capabilities(name));
    if ((capabilities.invocation_context || capabilities.idempotency_key ||
          capabilities.heartbeat) &&
        !invocation_aware) {
      capabilities.declaration_valid = false;
    }
    if (capabilities.compensation && !compensating) {
      capabilities.declaration_valid = false;
    }
    return capabilities;
  }
  else {
    return {
      .invocation_context = invocation_aware,
      .idempotency_key = false,
      .heartbeat = false,
      .compensation = compensating,
    };
  }
}

inline nlohmann::json tool_descriptor_to_json(const tool_descriptor& value) {
  validate_tool_descriptor(value);
  auto capabilities = nlohmann::json::array();
  for (const auto& requirement : value.capabilities) {
    capabilities.push_back({
      { "name", requirement.name },
      { "risk", capability::to_string(requirement.risk) },
      { "summary", requirement.summary },
      { "resources", requirement.resources },
      { "metadata", requirement.metadata },
    });
  }
  auto retryable_categories = nlohmann::json::array();
  for (const auto category : value.retry.retryable_categories) {
    retryable_categories.push_back(to_string(category));
  }
  return {
    { "schema_version", 3 },
    { "name", value.name },
    { "version", value.version },
    { "description", value.description },
    { "input_schema", value.input_schema },
    { "output_schema", value.output_schema },
    { "side_effect", to_string(value.side_effect) },
    { "idempotency", to_string(value.idempotency) },
    { "approval", to_string(value.approval) },
    { "timeout_ms", value.timeout.count() },
    { "output_validation", to_string(value.output_validation) },
    { "retry",
      {
        { "max_attempts", value.retry.max_attempts },
        { "initial_backoff_ms", value.retry.initial_backoff.count() },
        { "maximum_backoff_ms", value.retry.maximum_backoff.count() },
        { "backoff_multiplier", value.retry.backoff_multiplier },
        { "jitter_ratio", value.retry.jitter_ratio },
        { "retryable_categories", std::move(retryable_categories) },
      } },
    { "resource_version",
      {
        { "require_expected_version", value.resource_version.require_expected_version },
        { "argument_json_pointer", value.resource_version.argument_json_pointer },
        { "outcome_json_pointer", value.resource_version.outcome_json_pointer },
      } },
    { "compensation",
      {
        { "enabled", value.compensation.enabled },
        { "timeout_ms", value.compensation.timeout.count() },
        { "max_attempts", value.compensation.max_attempts },
      } },
    { "heartbeat",
      {
        { "timeout_ms", value.heartbeat.timeout.count() },
        { "minimum_interval_ms", value.heartbeat.minimum_interval.count() },
      } },
    { "capabilities", std::move(capabilities) },
    { "model_output_projection",
      {
        { "max_bytes",
          value.model_output_projection.max_bytes
            ? nlohmann::json(*value.model_output_projection.max_bytes)
            : nlohmann::json(nullptr) },
        { "max_tokens",
          value.model_output_projection.max_tokens
            ? nlohmann::json(*value.model_output_projection.max_tokens)
            : nlohmann::json(nullptr) },
      } },
    { "metadata", value.metadata },
  };
}

inline tool_descriptor descriptor_from_llm_tool(const llm_tool& value) {
  auto schema = nlohmann::json::parse(
    value.parameters_json_schema.empty() ? "{}" : value.parameters_json_schema, nullptr, false);
  if (schema.is_discarded()) {
    throw std::invalid_argument("tool '" + value.name + "' has an invalid JSON input schema");
  }
  tool_descriptor descriptor {
    .name = value.name,
    .description = value.description,
    .input_schema = std::move(schema),
  };
  validate_tool_descriptor(descriptor);
  return descriptor;
}

} // namespace wuwe::agent::tools

namespace wuwe {
using llm_tool_result = agent::tools::tool_outcome;
}

#endif // WUWE_AGENT_TOOLS_TOOL_CONTRACT_HPP
