#ifndef WUWE_AGENT_RUNTIME_SCHEDULER_HPP
#define WUWE_AGENT_RUNTIME_SCHEDULER_HPP

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include <wuwe/agent/runtime/executor.hpp>

namespace wuwe::agent::runtime {

using scheduler_clock = std::chrono::steady_clock;
using scheduled_work = executor_work;

class scheduler {
public:
  virtual ~scheduler() = default;

  [[nodiscard]] virtual scheduled_task schedule_at(
    scheduler_clock::time_point time, scheduled_work work) = 0;

  [[nodiscard]] virtual bool wait_until(
    scheduler_clock::time_point time, std::stop_token stop_token = {}) = 0;

  [[nodiscard]] scheduled_task schedule_after(
    scheduler_clock::duration delay, scheduled_work work) {
    if (delay < scheduler_clock::duration::zero()) {
      delay = scheduler_clock::duration::zero();
    }
    return schedule_at(scheduler_clock::now() + delay, std::move(work));
  }

  [[nodiscard]] bool wait_for(scheduler_clock::duration delay, std::stop_token stop_token = {}) {
    if (delay < scheduler_clock::duration::zero()) {
      delay = scheduler_clock::duration::zero();
    }
    return wait_until(scheduler_clock::now() + delay, stop_token);
  }
};

class timer_scheduler final : public scheduler {
public:
  explicit timer_scheduler(std::shared_ptr<executor> dispatch_executor = {})
      : state_(std::make_shared<timer_state>(std::move(dispatch_executor))),
        worker_([state = state_](std::stop_token stop_token) { run(state, stop_token); }) {
  }

  timer_scheduler(const timer_scheduler&) = delete;
  timer_scheduler& operator=(const timer_scheduler&) = delete;

  ~timer_scheduler() override {
    shutdown();
  }

  [[nodiscard]] scheduled_task schedule_at(
    scheduler_clock::time_point time, scheduled_work work) override {
    if (!work) {
      throw std::invalid_argument("scheduled work must not be empty");
    }
    scheduled_task_source source;
    const std::weak_ptr<timer_state> weak_state = state_;
    source.set_cancellation_notifier([weak_state] {
      if (const auto state = weak_state.lock())
        state->changed.notify_all();
    });
    {
      std::scoped_lock lock(state_->mutex);
      if (state_->stopping) {
        throw std::runtime_error("scheduler is shutting down");
      }
      prune_completed(state_->active);
      state_->queue.push(entry {
        .time = time,
        .sequence = state_->next_sequence++,
        .work = std::move(work),
        .source = source,
      });
    }
    state_->changed.notify_all();
    return source.task();
  }

  [[nodiscard]] bool wait_until(
    scheduler_clock::time_point time, std::stop_token stop_token = {}) override {
    std::mutex mutex;
    std::condition_variable_any wake;
    std::unique_lock lock(mutex);
    wake.wait_until(lock, stop_token, time, [] { return false; });
    return !stop_token.stop_requested() && scheduler_clock::now() >= time;
  }

  void shutdown() noexcept {
    std::vector<scheduled_task_source> abandoned;
    std::vector<scheduled_task> active;
    std::jthread timer_worker;
    {
      std::scoped_lock lifecycle_lock(shutdown_mutex_);
      {
        std::scoped_lock lock(state_->mutex);
        state_->stopping = true;
        while (!state_->queue.empty()) {
          abandoned.push_back(state_->queue.top().source);
          state_->queue.pop();
        }
        prune_completed(state_->active);
        active = state_->active;
      }
      timer_worker = std::move(worker_);
    }
    for (const auto& source : abandoned) {
      source.request_stop();
      (void)source.complete_without_running();
    }
    for (const auto& task : active)
      task.request_stop();

    const bool inside_dispatch_domain =
      state_->dispatch_executor && state_->dispatch_executor->owns_current_thread();

    timer_worker.request_stop();
    state_->changed.notify_all();
    const bool inside_timer_thread =
      timer_worker.joinable() && timer_worker.get_id() == std::this_thread::get_id();
    if (timer_worker.joinable()) {
      if (inside_timer_thread) {
        // run() owns timer_state, so self-destruction can safely detach.
        timer_worker.detach();
      }
      else {
        try {
          timer_worker.join();
        }
        catch (...) {
          if (timer_worker.joinable())
            timer_worker.detach();
        }
      }
    }

    if (!inside_timer_thread) {
      std::unique_lock lock(state_->mutex);
      state_->changed.wait(lock, [&] { return state_->timer_exited; });
    }

    // shutdown is a lifecycle barrier for callbacks already dispatched. A
    // callback may destroy its own scheduler; that one task is allowed to
    // finish naturally instead of waiting for itself.
    for (const auto& task : active) {
      if (!inside_dispatch_domain && !task.running_on_current_thread()) {
        try {
          task.wait();
        }
        catch (...) {
          // Task failures remain observable through their public handle.
        }
      }
    }
  }

private:
  struct entry {
    scheduler_clock::time_point time;
    std::size_t sequence { 0 };
    scheduled_work work;
    scheduled_task_source source;
  };

  struct later {
    bool operator()(const entry& lhs, const entry& rhs) const noexcept {
      if (lhs.time != rhs.time)
        return lhs.time > rhs.time;
      return lhs.sequence > rhs.sequence;
    }
  };

  struct timer_state {
    explicit timer_state(std::shared_ptr<executor> executor)
        : dispatch_executor(std::move(executor)) {
    }

    std::mutex mutex;
    std::condition_variable_any changed;
    std::priority_queue<entry, std::vector<entry>, later> queue;
    std::vector<scheduled_task> active;
    std::size_t next_sequence { 0 };
    bool stopping { false };
    bool timer_exited { false };
    std::shared_ptr<executor> dispatch_executor;
  };

  static void prune_completed(std::vector<scheduled_task>& tasks) {
    std::erase_if(tasks, [](const scheduled_task& task) { return task.done(); });
  }

  static void run(const std::shared_ptr<timer_state>& state, std::stop_token stop_token) noexcept {
    std::unique_lock lock(state->mutex);
    while (!stop_token.stop_requested()) {
      if (state->queue.empty()) {
        state->changed.wait(
          lock, stop_token, [&] { return state->stopping || !state->queue.empty(); });
      }
      if (state->stopping || stop_token.stop_requested())
        break;
      if (state->queue.empty())
        continue;

      if (state->queue.top().source.stop_token().stop_requested()) {
        auto cancelled = state->queue.top().source;
        state->queue.pop();
        lock.unlock();
        (void)cancelled.complete_without_running();
        lock.lock();
        continue;
      }

      const auto due = state->queue.top().time;
      state->changed.wait_until(lock, stop_token, due, [&] {
        return state->stopping || state->queue.empty() || state->queue.top().time < due ||
               state->queue.top().source.stop_token().stop_requested();
      });
      if (state->stopping || stop_token.stop_requested())
        break;
      if (state->queue.empty() || state->queue.top().time > scheduler_clock::now()) {
        continue;
      }

      auto item = state->queue.top();
      state->queue.pop();
      const auto scheduled = item.source.task();
      prune_completed(state->active);
      state->active.push_back(scheduled);
      if (!state->dispatch_executor) {
        state->dispatch_executor = default_executor();
      }
      const auto dispatch = state->dispatch_executor;
      lock.unlock();

      if (item.source.stop_token().stop_requested()) {
        (void)item.source.complete_without_running();
      }
      else {
        try {
          (void)dispatch->submit([source = item.source, work = std::move(item.work)](
                                   std::stop_token dispatch_stop) mutable {
            std::stop_callback cancel(dispatch_stop, [source] { source.request_stop(); });
            source.execute(std::move(work));
          });
        }
        catch (...) {
          (void)item.source.fail_without_running(std::current_exception());
        }
      }
      lock.lock();
    }
    state->timer_exited = true;
    lock.unlock();
    state->changed.notify_all();
  }

  std::shared_ptr<timer_state> state_;
  std::jthread worker_;
  std::mutex shutdown_mutex_;
};

[[nodiscard]] inline std::shared_ptr<scheduler> default_scheduler() {
  // If scheduled callbacks are used, timer_state retains the lazily selected
  // default executor until scheduler shutdown, independent of static teardown
  // order between the two accessors.
  static auto instance = std::make_shared<timer_scheduler>();
  return instance;
}

} // namespace wuwe::agent::runtime

#endif // WUWE_AGENT_RUNTIME_SCHEDULER_HPP
