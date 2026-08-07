#ifndef WUWE_AGENT_CORE_EXECUTION_OBSERVABILITY_HPP
#define WUWE_AGENT_CORE_EXECUTION_OBSERVABILITY_HPP

#include <map>
#include <string>
#include <utility>

#include <wuwe/agent/core/execution_context_projection.hpp>
#include <wuwe/agent/core/observability.hpp>

namespace wuwe::agent::observability {

[[nodiscard]] inline agent_event make_agent_event(const core::agent_execution_context& context,
  std::string module, std::string name, std::map<std::string, std::string> attributes = {},
  nlohmann::json data = {}) {
  core::apply_execution_context_attributes(attributes, context);
  return {
    .module = std::move(module),
    .name = std::move(name),
    .trace_id = context.trace_id,
    .subject_id = core::execution_context_subject_id(context),
    .run_id = context.run_id,
    .request_id = context.request_id,
    .attributes = std::move(attributes),
    .data = std::move(data),
  };
}

} // namespace wuwe::agent::observability

#endif // WUWE_AGENT_CORE_EXECUTION_OBSERVABILITY_HPP
