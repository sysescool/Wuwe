#include <wuwe/agent/llm/tool_output_projection.hpp>

bool tool_output_projection_header_is_independent() {
  const wuwe::agent::llm::tool_output_projection_policy policy;
  return policy.max_bytes != 0 && policy.max_tokens != 0;
}
