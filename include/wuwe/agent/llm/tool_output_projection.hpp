#ifndef WUWE_AGENT_LLM_TOOL_OUTPUT_PROJECTION_HPP
#define WUWE_AGENT_LLM_TOOL_OUTPUT_PROJECTION_HPP

#include <wuwe/agent/llm/text_token_estimator.hpp>
#include <wuwe/agent/llm/tool_output_projection_types.hpp>

namespace wuwe::agent::tools {
struct tool_outcome;
}

namespace wuwe::agent::llm {

void validate_tool_output_projection_policy(const tool_output_projection_policy& policy);

[[nodiscard]] tool_output_projection_policy tighten_tool_output_projection_policy(
  tool_output_projection_policy base, const tool_output_projection_policy& ceiling);

[[nodiscard]] tool_output_projection_policy effective_tool_output_projection_policy(
  tool_output_projection_policy base, const tool_output_projection_constraints& constraints);

[[nodiscard]] tool_output_projection_policy_validation check_tool_output_projection_policy(
  const tool_output_projection_policy& policy, const text_token_estimator& estimator);

[[nodiscard]] tool_output_projection_result try_project_tool_output_for_model(
  const tools::tool_outcome& result, const tool_output_projection_policy& policy,
  const text_token_estimator& estimator);

// Compatibility wrapper for callers that prefer exception-based validation.
// New runtime integrations should use try_project_tool_output_for_model().
[[nodiscard]] tool_output_projection project_tool_output_for_model(
  const tools::tool_outcome& result, const tool_output_projection_policy& policy,
  const text_token_estimator& estimator);

} // namespace wuwe::agent::llm

#endif // WUWE_AGENT_LLM_TOOL_OUTPUT_PROJECTION_HPP
