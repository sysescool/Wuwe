#ifndef WUWE_AGENT_CORE_EXECUTION_CONTEXT_PROJECTION_HPP
#define WUWE_AGENT_CORE_EXECUTION_CONTEXT_PROJECTION_HPP

#include <map>
#include <string>
#include <string_view>

#include <wuwe/agent/core/execution_context.hpp>

namespace wuwe::agent::core {

struct execution_context_projection_options {
  bool include_empty_identifiers { false };
  bool include_metadata { true };
  bool include_sensitive_metadata { false };
  std::string metadata_prefix { "context.metadata." };
};

inline void apply_execution_context_attributes(std::map<std::string, std::string>& target,
  const agent_execution_context& context,
  const execution_context_projection_options& options = {}) {
  const auto apply_identifier = [&](std::string_view name, const std::string& value) {
    if (options.include_empty_identifiers || !value.empty()) {
      target[std::string(name)] = value;
    }
  };

  apply_identifier("run_id", context.run_id);
  apply_identifier("trace_id", context.trace_id);
  apply_identifier("request_id", context.request_id);
  apply_identifier("tenant_id", context.tenant_id);
  apply_identifier("user_id", context.user_id);
  apply_identifier("application_id", context.application_id);
  apply_identifier("workspace_id", context.workspace_id);
  apply_identifier("conversation_id", context.conversation_id);
  apply_identifier("agent_id", context.agent_id);
  apply_identifier("locale", context.locale);

  if (!options.include_metadata) {
    return;
  }
  for (const auto& [key, value] : context.metadata) {
    if (!options.include_sensitive_metadata && sensitive_execution_context_metadata_key(key)) {
      continue;
    }
    target[options.metadata_prefix + key] = value;
  }
}

[[nodiscard]] inline std::map<std::string, std::string> execution_context_attributes(
  const agent_execution_context& context,
  const execution_context_projection_options& options = {}) {
  std::map<std::string, std::string> output;
  apply_execution_context_attributes(output, context, options);
  return output;
}

[[nodiscard]] inline const std::string& execution_context_subject_id(
  const agent_execution_context& context) noexcept {
  if (!context.user_id.empty()) {
    return context.user_id;
  }
  if (!context.agent_id.empty()) {
    return context.agent_id;
  }
  if (!context.application_id.empty()) {
    return context.application_id;
  }
  return context.run_id;
}

} // namespace wuwe::agent::core

#endif // WUWE_AGENT_CORE_EXECUTION_CONTEXT_PROJECTION_HPP
