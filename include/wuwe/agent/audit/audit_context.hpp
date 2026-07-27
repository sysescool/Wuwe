#ifndef WUWE_AGENT_AUDIT_AUDIT_CONTEXT_HPP
#define WUWE_AGENT_AUDIT_AUDIT_CONTEXT_HPP

#include <map>
#include <string>
#include <utility>

#include <wuwe/agent/audit/audit.hpp>
#include <wuwe/agent/core/execution_context_projection.hpp>

namespace wuwe::agent::audit {

[[nodiscard]] inline audit_event make_audit_event(
  const core::agent_execution_context& context,
  std::string module,
  std::string name,
  std::string id,
  audit_event_outcome outcome = audit_event_outcome::attempted,
  std::map<std::string, std::string> attributes = {}) {
  core::apply_execution_context_attributes(attributes, context);
  return {
    .module = std::move(module),
    .name = std::move(name),
    .id = std::move(id),
    .trace_id = context.trace_id,
    .subject_id = core::execution_context_subject_id(context),
    .outcome = outcome,
    .attributes = std::move(attributes),
  };
}

} // namespace wuwe::agent::audit

#endif // WUWE_AGENT_AUDIT_AUDIT_CONTEXT_HPP
