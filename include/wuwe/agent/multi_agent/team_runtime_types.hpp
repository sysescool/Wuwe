#ifndef WUWE_AGENT_MULTI_AGENT_TEAM_RUNTIME_TYPES_HPP
#define WUWE_AGENT_MULTI_AGENT_TEAM_RUNTIME_TYPES_HPP

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <wuwe/agent/core/observability.hpp>
#include <wuwe/agent/multi_agent/agent_registry.hpp>
#include <wuwe/agent/multi_agent/team_session.hpp>

namespace wuwe::agent::multi_agent {

using team_telemetry_failure_mode = observability::telemetry_failure_mode;

struct team_runtime_options {
  std::shared_ptr<agent_registry> registry;
  team_observer observer;
  observability::event_sink* event_sink {};
  team_telemetry_failure_mode telemetry_failure_mode {
    team_telemetry_failure_mode::ignore
  };
  std::size_t max_parallel_tasks { 4 };
  std::chrono::milliseconds default_task_timeout { 0 };
  std::chrono::milliseconds cancellation_poll_interval { 10 };
};

using consensus_resolver = std::function<agent_task_result(
  const std::vector<agent_task_result>&,
  const team_session_snapshot&)>;

struct consensus_request {
  agent_task_request task;
  std::vector<std::string> participant_agents;
  std::size_t minimum_successful_agents { 1 };
  std::size_t minimum_agreement { 0 };
  consensus_resolver resolver;
};

struct consensus_result {
  bool completed { false };
  agent_task_error_code error_code { agent_task_error_code::none };
  std::string error;
  std::vector<agent_task_result> results;
  agent_task_result final_result;

  [[nodiscard]] explicit operator bool() const noexcept {
    return completed && error_code == agent_task_error_code::none;
  }
};

} // namespace wuwe::agent::multi_agent

#endif // WUWE_AGENT_MULTI_AGENT_TEAM_RUNTIME_TYPES_HPP
