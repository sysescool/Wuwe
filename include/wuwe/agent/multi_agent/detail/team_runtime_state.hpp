#ifndef WUWE_AGENT_MULTI_AGENT_DETAIL_TEAM_RUNTIME_STATE_HPP
#define WUWE_AGENT_MULTI_AGENT_DETAIL_TEAM_RUNTIME_STATE_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <wuwe/agent/multi_agent/team_runtime_types.hpp>

namespace wuwe::agent::multi_agent::detail {

inline std::string next_team_id(std::string_view prefix) {
  static std::atomic<std::uint64_t> sequence { 0 };
  return std::string(prefix) + "-" + std::to_string(++sequence);
}

struct team_runtime_state {
  explicit team_runtime_state(team_runtime_options value)
      : options(std::move(value)) {
    if (!options.registry) options.registry = std::make_shared<agent_registry>();
    if (options.max_parallel_tasks == 0) {
      throw std::invalid_argument("team max_parallel_tasks must be greater than zero");
    }
    if (options.default_task_timeout.count() < 0) {
      throw std::invalid_argument("team default task timeout must not be negative");
    }
    if (options.cancellation_poll_interval.count() <= 0) {
      throw std::invalid_argument(
        "team cancellation poll interval must be greater than zero");
    }
  }

  team_runtime_options options;
  mutable std::mutex sessions_mutex;
  std::map<std::string, std::shared_ptr<team_session>> sessions;
  mutable std::mutex concurrency_mutex;
  std::condition_variable_any concurrency_condition;
  std::size_t active_runtime_tasks { 0 };
};

class team_runtime_slot {
public:
  team_runtime_slot() = default;
  explicit team_runtime_slot(std::shared_ptr<team_runtime_state> state)
      : state_(std::move(state)) {
  }
  team_runtime_slot(const team_runtime_slot&) = delete;
  team_runtime_slot& operator=(const team_runtime_slot&) = delete;
  team_runtime_slot(team_runtime_slot&& other) noexcept
      : state_(std::move(other.state_)) {
  }
  team_runtime_slot& operator=(team_runtime_slot&& other) noexcept {
    if (this != &other) {
      release();
      state_ = std::move(other.state_);
    }
    return *this;
  }
  ~team_runtime_slot() { release(); }

private:
  void release() noexcept {
    if (!state_) return;
    {
      std::scoped_lock lock(state_->concurrency_mutex);
      if (state_->active_runtime_tasks != 0) --state_->active_runtime_tasks;
    }
    state_->concurrency_condition.notify_one();
    state_.reset();
  }

  std::shared_ptr<team_runtime_state> state_;
};

class active_team_task_guard {
public:
  active_team_task_guard(
    std::shared_ptr<team_session> session,
    std::string task_id)
      : session_(std::move(session)), task_id_(std::move(task_id)) {
  }

  active_team_task_guard(const active_team_task_guard&) = delete;
  active_team_task_guard& operator=(const active_team_task_guard&) = delete;

  ~active_team_task_guard() {
    try {
      if (session_) (void)session_->fail_task_if_active(task_id_);
    }
    catch (...) {
    }
  }

private:
  std::shared_ptr<team_session> session_;
  std::string task_id_;
};

struct agent_execution_outcome {
  agent_task_result result;
  bool detached { false };
};

} // namespace wuwe::agent::multi_agent::detail

#endif // WUWE_AGENT_MULTI_AGENT_DETAIL_TEAM_RUNTIME_STATE_HPP
