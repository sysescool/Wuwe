#include <wuwe/agent/sandbox/sandbox_plan.hpp>

bool sandbox_plan_header_is_independent() {
  return wuwe::agent::sandbox::to_string(
           wuwe::agent::sandbox::sandbox_compile_error::unsupported_policy) == "unsupported_policy";
}
