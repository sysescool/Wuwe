#ifndef WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_EXECUTION_PLAN_MACOS_HPP
#define WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_EXECUTION_PLAN_MACOS_HPP

#ifdef __APPLE__
#include "restricted_process_sandbox_plan_macos.hpp"
#include <memory>
#include <stop_token>
#include <wuwe/agent/execution/execution_core.hpp>

namespace wuwe::agent::execution::detail {
[[nodiscard]] execution_result run_macos_restricted_execution_plan(
  std::shared_ptr<const macos_restricted_process_sandbox_plan> plan,
  const execution_request& request, std::stop_token stop_token = {});
}
#endif
#endif
