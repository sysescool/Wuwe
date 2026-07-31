#ifndef WUWE_AGENT_MCP_ASYNC_HPP
#define WUWE_AGENT_MCP_ASYNC_HPP

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/mcp/mcp_lifecycle.hpp>

namespace wuwe::agent::mcp {

struct mcp_async_cancel_token {
  std::shared_ptr<std::atomic_bool> cancelled;

  bool is_cancelled() const noexcept {
    return cancelled && cancelled->load();
  }
};

struct mcp_async_task_snapshot {
  mcp_request_record record;
  bool ready { false };
};

class mcp_async_task_registry {
public:
  template<typename Function>
  std::string submit(std::string id, std::string method, std::string target, json params,
    Function&& function, std::chrono::milliseconds timeout = std::chrono::milliseconds { 0 }) {
    if (id.empty()) {
      throw std::invalid_argument("MCP async task id must not be empty");
    }
    if (timeout.count() < 0) {
      throw std::invalid_argument("MCP async task timeout must not be negative");
    }
    auto data = std::make_shared<task_data>();
    data->cancelled = std::make_shared<std::atomic_bool>(false);
    data->record.id = id;
    data->record.method = std::move(method);
    data->record.target = std::move(target);
    data->record.params = std::move(params);
    data->record.state = mcp_request_state::running;
    data->record.started_at = std::chrono::system_clock::now();
    data->record.timeout = timeout;
    data->started_at = std::chrono::steady_clock::now();

    const auto token = mcp_async_cancel_token { data->cancelled };
    auto state = std::make_shared<task_state>();
    state->data = data;
    std::lock_guard registry_lock(mutex_);
    if (tasks_.contains(id)) {
      throw std::invalid_argument("duplicate MCP async task id: " + id);
    }
    const auto inserted = tasks_.emplace(id, state).first;
    try {
      state->future = std::async(
        std::launch::async, [data, token, function = std::forward<Function>(function)]() mutable {
          try {
            function(token);
            std::lock_guard lock(data->mutex);
            if (data->record.state == mcp_request_state::running) {
              data->record.state =
                token.is_cancelled() ? mcp_request_state::cancelled : mcp_request_state::completed;
              data->record.finished_at = std::chrono::system_clock::now();
            }
          }
          catch (const std::exception& ex) {
            std::lock_guard lock(data->mutex);
            if (data->record.state == mcp_request_state::running) {
              data->record.state = mcp_request_state::failed;
              data->record.error = ex.what();
              data->record.finished_at = std::chrono::system_clock::now();
            }
          }
          catch (...) {
            std::lock_guard lock(data->mutex);
            if (data->record.state == mcp_request_state::running) {
              data->record.state = mcp_request_state::failed;
              data->record.error = "unknown async task failure";
              data->record.finished_at = std::chrono::system_clock::now();
            }
          }
        });
    }
    catch (...) {
      tasks_.erase(inserted);
      throw;
    }
    return id;
  }

  bool cancel(const std::string& id, std::string reason = {}) {
    const auto task = find_task(id);
    if (!task) {
      return false;
    }
    task->data->cancelled->store(true);
    std::lock_guard lock(task->data->mutex);
    if (task->data->record.state == mcp_request_state::running) {
      task->data->record.state = mcp_request_state::cancelled;
      task->data->record.error = std::move(reason);
      task->data->record.finished_at = std::chrono::system_clock::now();
    }
    return true;
  }

  void progress(const std::string& id, json progress_token, double value,
    std::optional<double> total = std::nullopt, std::string message = {}) {
    const auto task = find_task(id);
    if (!task) {
      return;
    }
    std::lock_guard lock(task->data->mutex);
    task->data->record.progress_token = std::move(progress_token);
    task->data->record.progress = value;
    task->data->record.total = total;
    task->data->record.progress_message = std::move(message);
  }

  std::optional<mcp_async_task_snapshot> poll(const std::string& id) {
    const auto task = find_task(id);
    if (!task) {
      return std::nullopt;
    }
    enforce_timeout(*task->data);
    std::lock_guard lock(task->data->mutex);
    return mcp_async_task_snapshot {
      .record = task->data->record,
      .ready = task->future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready,
    };
  }

  std::vector<mcp_async_task_snapshot> snapshots() {
    std::vector<std::shared_ptr<task_state>> tasks;
    {
      std::lock_guard lock(mutex_);
      tasks.reserve(tasks_.size());
      for (const auto& [_, task] : tasks_) {
        tasks.push_back(task);
      }
    }

    std::vector<mcp_async_task_snapshot> output;
    output.reserve(tasks.size());
    for (const auto& task : tasks) {
      enforce_timeout(*task->data);
      std::lock_guard lock(task->data->mutex);
      output.push_back({
        .record = task->data->record,
        .ready = task->future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready,
      });
    }
    return output;
  }

  void clear_finished() {
    std::lock_guard lock(mutex_);
    for (auto it = tasks_.begin(); it != tasks_.end();) {
      const auto& task = it->second;
      std::lock_guard task_lock(task->data->mutex);
      const auto ready =
        task->future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
      if (ready && (task->data->record.state == mcp_request_state::completed ||
                     task->data->record.state == mcp_request_state::failed ||
                     task->data->record.state == mcp_request_state::cancelled)) {
        it = tasks_.erase(it);
      }
      else {
        ++it;
      }
    }
  }

private:
  struct task_data {
    mutable std::mutex mutex;
    mcp_request_record record;
    std::shared_ptr<std::atomic_bool> cancelled;
    std::chrono::steady_clock::time_point started_at;
  };

  struct task_state {
    std::shared_ptr<task_data> data;
    std::future<void> future;
  };

  std::shared_ptr<task_state> find_task(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) {
      return {};
    }
    return it->second;
  }

  static void enforce_timeout(task_data& task) {
    std::lock_guard lock(task.mutex);
    if (task.record.timeout.count() <= 0 || task.record.state != mcp_request_state::running) {
      return;
    }
    if (std::chrono::steady_clock::now() - task.started_at <= task.record.timeout) {
      return;
    }
    task.cancelled->store(true);
    task.record.state = mcp_request_state::failed;
    task.record.error = "request timed out";
    task.record.finished_at = std::chrono::system_clock::now();
  }

  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<task_state>> tasks_;
};

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_ASYNC_HPP
