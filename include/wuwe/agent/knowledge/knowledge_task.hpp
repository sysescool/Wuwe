#ifndef WUWE_AGENT_KNOWLEDGE_TASK_HPP
#define WUWE_AGENT_KNOWLEDGE_TASK_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wuwe::agent::knowledge {

enum class knowledge_task_state {
  pending,
  running,
  completed,
  failed,
  canceled,
};

struct knowledge_task_progress {
  knowledge_task_state state { knowledge_task_state::pending };
  std::size_t completed {};
  std::size_t total {};
  std::string message;
  std::vector<std::string> errors;
};

using knowledge_task_progress_callback = std::function<void(const knowledge_task_progress&)>;

struct knowledge_task_policy {
  std::size_t max_retries {};
  std::chrono::milliseconds retry_backoff { 50 };
};

template<typename Result>
class knowledge_task {
public:
  explicit knowledge_task(std::future<Result> future) : future_(std::move(future)) {
  }

  Result get() {
    return future_.get();
  }

  bool ready() const {
    return future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
  }

  knowledge_task_progress progress() const {
    std::scoped_lock lock(mutex_);
    return progress_;
  }

  void update_progress(knowledge_task_progress progress) {
    std::scoped_lock lock(mutex_);
    progress_ = std::move(progress);
  }

  void request_cancel() {
    cancel_requested_.store(true);
    cancellation_condition_.notify_all();
  }

  bool cancel_requested() const {
    return cancel_requested_.load();
  }

  bool wait_for_cancel(std::chrono::milliseconds duration) const {
    if (cancel_requested())
      return true;
    std::unique_lock lock(cancellation_mutex_);
    return cancellation_condition_.wait_for(lock, duration, [&] { return cancel_requested(); });
  }

private:
  std::future<Result> future_;
  mutable std::mutex mutex_;
  knowledge_task_progress progress_;
  std::atomic<bool> cancel_requested_ { false };
  mutable std::mutex cancellation_mutex_;
  mutable std::condition_variable cancellation_condition_;
};

} // namespace wuwe::agent::knowledge

#endif // WUWE_AGENT_KNOWLEDGE_TASK_HPP
