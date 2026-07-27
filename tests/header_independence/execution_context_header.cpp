#include <wuwe/agent/core/execution_context.hpp>

bool execution_context_header_is_independent() {
  return !wuwe::agent::core::agent_execution_context {}.interrupted();
}
