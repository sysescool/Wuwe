#ifndef WUWE_AGENT_CORE_EXECUTION_CONTEXT_HPP
#define WUWE_AGENT_CORE_EXECUTION_CONTEXT_HPP

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <wuwe/agent/core/metadata.hpp>

namespace wuwe::agent::core {

struct execution_context_serialization_options {
  bool include_sensitive_metadata { false };
};

struct agent_execution_context {
  std::string run_id;
  std::string trace_id;
  std::string request_id;

  std::string tenant_id;
  std::string user_id;
  std::string application_id;
  std::string workspace_id;
  std::string conversation_id;
  std::string agent_id;
  std::string locale;

  std::optional<std::chrono::system_clock::time_point> deadline;
  std::stop_token stop_token;
  std::map<std::string, std::string> metadata;

  [[nodiscard]] bool cancellation_requested() const noexcept {
    return stop_token.stop_requested();
  }

  [[nodiscard]] bool deadline_reached(
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const noexcept {
    return deadline && now >= *deadline;
  }

  [[nodiscard]] bool interrupted() const noexcept {
    return cancellation_requested() || deadline_reached();
  }

  [[nodiscard]] std::optional<std::chrono::milliseconds> remaining_time(
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const noexcept {
    if (!deadline) {
      return std::nullopt;
    }
    if (now >= *deadline) {
      return std::chrono::milliseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
  }
};

inline nlohmann::json execution_context_to_json(
  const agent_execution_context& context, execution_context_serialization_options options = {}) {
  auto metadata = context.metadata;
  if (!options.include_sensitive_metadata) {
    std::erase_if(metadata,
      [](const auto& item) { return sensitive_execution_context_metadata_key(item.first); });
  }
  nlohmann::json output {
    { "run_id", context.run_id },
    { "trace_id", context.trace_id },
    { "request_id", context.request_id },
    { "tenant_id", context.tenant_id },
    { "user_id", context.user_id },
    { "application_id", context.application_id },
    { "workspace_id", context.workspace_id },
    { "conversation_id", context.conversation_id },
    { "agent_id", context.agent_id },
    { "locale", context.locale },
    { "metadata", std::move(metadata) },
  };
  if (context.deadline) {
    output["deadline_unix_ms"] =
      std::chrono::duration_cast<std::chrono::milliseconds>(context.deadline->time_since_epoch())
        .count();
  }
  else {
    output["deadline_unix_ms"] = nullptr;
  }
  return output;
}

inline agent_execution_context execution_context_from_json(const nlohmann::json& value) {
  agent_execution_context context;
  context.run_id = value.value("run_id", std::string {});
  context.trace_id = value.value("trace_id", std::string {});
  context.request_id = value.value("request_id", std::string {});
  context.tenant_id = value.value("tenant_id", std::string {});
  context.user_id = value.value("user_id", std::string {});
  context.application_id = value.value("application_id", std::string {});
  context.workspace_id = value.value("workspace_id", std::string {});
  context.conversation_id = value.value("conversation_id", std::string {});
  context.agent_id = value.value("agent_id", std::string {});
  context.locale = value.value("locale", std::string {});
  context.metadata = value.value("metadata", std::map<std::string, std::string> {});
  if (value.contains("deadline_unix_ms") && !value.at("deadline_unix_ms").is_null()) {
    context.deadline = std::chrono::system_clock::time_point(
      std::chrono::milliseconds(value.at("deadline_unix_ms").get<std::int64_t>()));
  }
  return context;
}

} // namespace wuwe::agent::core

#endif // WUWE_AGENT_CORE_EXECUTION_CONTEXT_HPP
