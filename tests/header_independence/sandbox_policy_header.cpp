#include <wuwe/agent/sandbox/sandbox_policy.hpp>

bool sandbox_policy_header_is_independent() {
  wuwe::agent::sandbox::sandbox_policy policy;
  return policy.network.mode == wuwe::agent::sandbox::sandbox_network_mode::denied;
}
