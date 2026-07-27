#ifndef WUWE_AGENT_RUNTIME_EXECUTOR_HPP
#define WUWE_AGENT_RUNTIME_EXECUTOR_HPP

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace wuwe::agent::runtime {

using executor_work = std::function<void(std::stop_token)>;

namespace detail {

struct scheduled_task_state {
  std::stop_source stop_source;
  mutable std::mutex mutex;
  std::condition_variable completed;
  bool started { false };
  bool done { false };
  std::thread::id execution_thread;
  std::exception_ptr failure;
  std::function<void()> cancellation_notifier;

  void request_stop() noexcept {
    stop_source.request_stop();
    std::function<void()> notify;
    {
      std::scoped_lock lock(mutex);
      if (!done) notify = cancellation_notifier;
    }
    if (notify) notify();
  }

  bool begin() noexcept {
    std::scoped_lock lock(mutex);
    if (started || done) return false;
    started = true;
    execution_thread = std::this_thread::get_id();
    cancellation_notifier = {};
    return true;
  }

  bool finish_without_running(std::exception_ptr error = {}) noexcept {
    {
      std::scoped_lock lock(mutex);
      if (started || done) return false;
      failure = std::move(error);
      cancellation_notifier = {};
      done = true;
    }
    completed.notify_all();
    return true;
  }

  void finish(std::exception_ptr error = {}) noexcept {
    {
      std::scoped_lock lock(mutex);
      if (done) return;
      failure = std::move(error);
      cancellation_notifier = {};
      execution_thread = {};
      done = true;
    }
    completed.notify_all();
  }
};

inline thread_local const void* current_executor_domain {};

} // namespace detail

class scheduled_task {
public:
  scheduled_task() = default;

  void request_stop() const noexcept {
    if (state_) state_->request_stop();
  }

  [[nodiscard]] bool stop_requested() const noexcept {
    return state_ && state_->stop_source.stop_requested();
  }

  [[nodiscard]] bool valid() const noexcept {
    return static_cast<bool>(state_);
  }

  [[nodiscard]] bool done() const noexcept {
    if (!state_) return true;
    std::scoped_lock lock(state_->mutex);
    return state_->done;
  }

  [[nodiscard]] bool running_on_current_thread() const noexcept {
    if (!state_) return false;
    std::scoped_lock lock(state_->mutex);
    return !state_->done && state_->started &&
      state_->execution_thread == std::this_thread::get_id();
  }

  void wait() const {
    if (!state_) return;
    std::unique_lock lock(state_->mutex);
    if (!state_->done && state_->started &&
        state_->execution_thread == std::this_thread::get_id()) {
      throw std::logic_error("a scheduled task cannot wait for itself");
    }
    state_->completed.wait(lock, [&] { return state_->done; });
  }

  [[nodiscard]] bool wait_for(std::chrono::milliseconds timeout) const {
    if (!state_) return true;
    std::unique_lock lock(state_->mutex);
    if (!state_->done && state_->started &&
        state_->execution_thread == std::this_thread::get_id()) {
      throw std::logic_error("a scheduled task cannot wait for itself");
    }
    return state_->completed.wait_for(lock, timeout, [&] { return state_->done; });
  }

  void rethrow_if_failed() const {
    if (!state_) return;
    wait();
    std::exception_ptr failure;
    {
      std::scoped_lock lock(state_->mutex);
      failure = state_->failure;
    }
    if (failure) std::rethrow_exception(failure);
  }

private:
  explicit scheduled_task(std::shared_ptr<detail::scheduled_task_state> state)
      : state_(std::move(state)) {
  }

  std::shared_ptr<detail::scheduled_task_state> state_;

  friend class scheduled_task_source;
};

// A public completion source lets custom executors and schedulers produce fully
// functional scheduled_task handles without exposing task-state internals.
// execute() owns the running-thread bookkeeping used to reject self-waits.
class scheduled_task_source {
public:
  scheduled_task_source()
      : state_(std::make_shared<detail::scheduled_task_state>()) {
  }

  [[nodiscard]] scheduled_task task() const {
    return scheduled_task(state_);
  }

  [[nodiscard]] std::stop_token stop_token() const noexcept {
    return state_->stop_source.get_token();
  }

  void request_stop() const noexcept {
    state_->request_stop();
  }

  void set_cancellation_notifier(std::function<void()> notifier) const {
    std::scoped_lock lock(state_->mutex);
    if (!state_->done) state_->cancellation_notifier = std::move(notifier);
  }

  [[nodiscard]] bool complete_without_running() const noexcept {
    return state_->finish_without_running();
  }

  [[nodiscard]] bool fail_without_running(std::exception_ptr failure) const noexcept {
    return state_->finish_without_running(std::move(failure));
  }

  void execute(const executor_work& work) const noexcept {
    if (!state_->begin()) return;
    try {
      work(state_->stop_source.get_token());
      state_->finish();
    }
    catch (...) {
      state_->finish(std::current_exception());
    }
  }

  void execute(executor_work&& work) const noexcept {
    execute(static_cast<const executor_work&>(work));
  }

private:
  std::shared_ptr<detail::scheduled_task_state> state_;
};

class executor {
public:
  virtual ~executor() = default;
  [[nodiscard]] virtual scheduled_task submit(executor_work work) = 0;
  [[nodiscard]] virtual std::size_t concurrency() const noexcept = 0;

  // Executors sharing one bounded worker domain should return the same value.
  // This lets blocking orchestration reject nested submissions that could starve
  // the domain. The default is suitable for independent custom executors.
  [[nodiscard]] virtual const void* execution_domain() const noexcept {
    return this;
  }

  // Custom executors should override this when work may execute on a thread
  // owned by their execution domain.
  [[nodiscard]] virtual bool owns_current_thread() const noexcept {
    return false;
  }
};

struct thread_pool_options {
  std::size_t threads {
    (std::min)(std::size_t { 16 },
      (std::max)(std::size_t { 1 },
        static_cast<std::size_t>(std::thread::hardware_concurrency())))
  };
  std::size_t queue_capacity { 1024 };
};

class thread_pool_executor final : public executor {
public:
  explicit thread_pool_executor(thread_pool_options options = {})
      : options_(options), state_(std::make_shared<pool_state>()) {
    if (options_.threads == 0) {
      throw std::invalid_argument("thread pool requires at least one worker");
    }
    if (options_.queue_capacity == 0) {
      throw std::invalid_argument("thread pool queue capacity must be positive");
    }
    state_->queue_capacity = options_.queue_capacity;
    workers_.reserve(options_.threads);
    try {
      for (std::size_t index = 0; index < options_.threads; ++index) {
        workers_.emplace_back([state = state_](std::stop_token stop_token) {
          worker_loop(state, stop_token);
        });
        {
          std::scoped_lock lock(state_->mutex);
          ++state_->live_workers;
        }
      }
    }
    catch (...) {
      shutdown();
      throw;
    }
  }

  thread_pool_executor(const thread_pool_executor&) = delete;
  thread_pool_executor& operator=(const thread_pool_executor&) = delete;

  ~thread_pool_executor() override {
    shutdown();
  }

  [[nodiscard]] scheduled_task submit(executor_work work) override {
    if (!work) {
      throw std::invalid_argument("executor work must not be empty");
    }
    scheduled_task_source source;
    const std::weak_ptr<pool_state> weak_state = state_;
    source.set_cancellation_notifier([weak_state] {
      if (const auto state = weak_state.lock()) state->ready.notify_one();
    });
    {
      std::scoped_lock lock(state_->mutex);
      if (state_->stopping) {
        throw std::runtime_error("executor is shutting down");
      }
      if (state_->queue.size() >= state_->queue_capacity) {
        throw std::runtime_error("executor queue capacity is exhausted");
      }
      state_->queue.push_back({ std::move(work), source });
    }
    state_->ready.notify_one();
    return source.task();
  }

  [[nodiscard]] std::size_t concurrency() const noexcept override {
    return options_.threads;
  }

  [[nodiscard]] const void* execution_domain() const noexcept override {
    return state_.get();
  }

  [[nodiscard]] bool owns_current_thread() const noexcept override {
    return detail::current_executor_domain == state_.get();
  }

  [[nodiscard]] std::size_t queued() const noexcept {
    std::scoped_lock lock(state_->mutex);
    return state_->queue.size();
  }

  void shutdown() noexcept {
    std::vector<std::jthread> workers;
    {
      std::scoped_lock lifecycle_lock(shutdown_mutex_);
      {
        std::scoped_lock lock(state_->mutex);
        state_->stopping = true;
      }
      workers.swap(workers_);
    }
    state_->ready.notify_all();
    for (auto& worker : workers) worker.request_stop();

    if (owns_current_thread()) {
      // Never join another worker from inside the same bounded domain: that
      // worker may itself be entering shutdown. Shared pool state keeps all
      // detached workers and accepted work alive until the domain drains.
      for (auto& worker : workers) {
        if (worker.joinable()) worker.detach();
      }
      return;
    }

    for (auto& worker : workers) {
      if (!worker.joinable()) continue;
      try {
        worker.join();
      }
      catch (...) {
        // shutdown() is noexcept. A still-joinable worker remains safe to detach
        // because its closure owns the shared pool state.
        if (worker.joinable()) worker.detach();
      }
    }
    std::unique_lock lock(state_->mutex);
    state_->workers_completed.wait(lock, [&] {
      return state_->live_workers == 0;
    });
  }

private:
  struct work_item {
    executor_work work;
    scheduled_task_source source;
  };

  struct pool_state {
    std::mutex mutex;
    std::condition_variable_any ready;
    std::deque<work_item> queue;
    std::size_t queue_capacity { 0 };
    std::size_t live_workers { 0 };
    bool stopping { false };
    std::condition_variable workers_completed;
  };

  static void worker_loop(
    const std::shared_ptr<pool_state>& state,
    std::stop_token worker_stop_token) noexcept {
    const auto previous_domain = detail::current_executor_domain;
    detail::current_executor_domain = state.get();
    while (true) {
      work_item item;
      {
        std::unique_lock lock(state->mutex);
        state->ready.wait(lock, worker_stop_token, [&] {
          return state->stopping || !state->queue.empty();
        });
        if ((state->stopping || worker_stop_token.stop_requested()) &&
            state->queue.empty()) {
          break;
        }
        if (state->queue.empty()) continue;
        item = std::move(state->queue.front());
        state->queue.pop_front();
      }
      item.source.execute(std::move(item.work));
    }
    detail::current_executor_domain = previous_domain;
    {
      std::scoped_lock lock(state->mutex);
      if (state->live_workers != 0) --state->live_workers;
    }
    state->workers_completed.notify_all();
  }

  thread_pool_options options_;
  std::shared_ptr<pool_state> state_;
  std::vector<std::jthread> workers_;
  std::mutex shutdown_mutex_;
};

[[nodiscard]] inline std::shared_ptr<executor> default_executor() {
  static auto instance = std::make_shared<thread_pool_executor>();
  return instance;
}

[[nodiscard]] inline std::shared_ptr<executor> default_tool_executor() {
  static auto instance = std::make_shared<thread_pool_executor>(
    thread_pool_options { .threads = (std::min)(std::size_t { 32 },
                            (std::max)(std::size_t { 4 },
                              static_cast<std::size_t>(
                                std::thread::hardware_concurrency()))),
                          .queue_capacity = 1024 });
  return instance;
}

} // namespace wuwe::agent::runtime

#endif // WUWE_AGENT_RUNTIME_EXECUTOR_HPP
