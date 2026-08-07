#include <wuwe/agent/process/process.hpp>

bool process_tools_header_is_independent() {
  return !wuwe::agent::process::process_policy {}.allow_shell;
}
