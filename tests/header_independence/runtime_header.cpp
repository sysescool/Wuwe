#include <wuwe/agent/runtime/runtime.hpp>

bool runtime_header_is_independent() {
  return wuwe::agent::runtime::terminal(
    wuwe::agent::runtime::agent_run_status::completed);
}
