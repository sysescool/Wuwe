#include <wuwe/agent/multi_agent/multi_agent.hpp>

#ifdef WUWE_AGENT_PLANNING_PLAN_HPP
#error "multi_agent.hpp must not force the optional Planning adapter"
#endif

bool multi_agent_header_is_independent() {
  wuwe::agent::multi_agent::team_runtime runtime;
  return runtime.registry() != nullptr;
}
