#ifndef WUWE_AGENT_CORE_OBSERVABILITY_HPP
#define WUWE_AGENT_CORE_OBSERVABILITY_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace wuwe::agent::observability {

enum class telemetry_failure_mode {
  ignore,
  propagate,
};

template<typename Callback>
[[nodiscard]] bool invoke_telemetry(
  telemetry_failure_mode mode,
  Callback&& callback) {
  try {
    std::forward<Callback>(callback)();
    return true;
  }
  catch (...) {
    if (mode == telemetry_failure_mode::propagate) throw;
    return false;
  }
}

struct agent_event {
  std::uint32_t schema_version { 1 };
  std::string module;
  std::string name;
  std::string trace_id;
  std::string subject_id;
  std::string run_id;
  std::uint64_t sequence { 0 };
  std::string request_id;
  std::string step_id;
  std::string tool_call_id;
  std::chrono::system_clock::time_point timestamp { std::chrono::system_clock::now() };
  std::chrono::milliseconds elapsed { 0 };
  std::map<std::string, std::string> attributes;
  nlohmann::json data;
};

inline nlohmann::json agent_event_to_json(const agent_event& event) {
  const auto timestamp_ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(
      event.timestamp.time_since_epoch()).count();
  return {
    { "schemaVersion", event.schema_version },
    { "module", event.module },
    { "name", event.name },
    { "traceId", event.trace_id },
    { "subjectId", event.subject_id },
    { "runId", event.run_id },
    { "sequence", event.sequence },
    { "requestId", event.request_id },
    { "stepId", event.step_id },
    { "toolCallId", event.tool_call_id },
    { "timestampUnixMillis", timestamp_ms },
    { "elapsedMillis", event.elapsed.count() },
    { "attributes", event.attributes },
    { "data", event.data },
  };
}

inline agent_event agent_event_from_json(const nlohmann::json& value) {
  const auto schema_version = value.value("schemaVersion", std::uint32_t { 1 });
  if (schema_version != 1) {
    throw std::invalid_argument("unsupported agent event schema version");
  }
  agent_event event;
  event.schema_version = schema_version;
  event.module = value.value("module", std::string {});
  event.name = value.value("name", std::string {});
  event.trace_id = value.value("traceId", std::string {});
  event.subject_id = value.value("subjectId", std::string {});
  event.run_id = value.value("runId", std::string {});
  event.sequence = value.value("sequence", std::uint64_t {});
  event.request_id = value.value("requestId", std::string {});
  event.step_id = value.value("stepId", std::string {});
  event.tool_call_id = value.value("toolCallId", std::string {});
  event.timestamp = std::chrono::system_clock::time_point(
    std::chrono::milliseconds(value.value("timestampUnixMillis", std::int64_t {})));
  event.elapsed = std::chrono::milliseconds(
    value.value("elapsedMillis", std::int64_t {}));
  event.attributes = value.value(
    "attributes", std::map<std::string, std::string> {});
  event.data = value.value("data", nlohmann::json {});
  return event;
}

class event_sink {
public:
  virtual ~event_sink() = default;
  virtual void publish(const agent_event& event) = 0;
};

class in_memory_event_sink final : public event_sink {
public:
  void publish(const agent_event& event) override {
    std::scoped_lock lock(mutex_);
    events_.push_back(event);
  }

  std::vector<agent_event> events() const {
    std::scoped_lock lock(mutex_);
    return events_;
  }

  void clear() {
    std::scoped_lock lock(mutex_);
    events_.clear();
  }

private:
  mutable std::mutex mutex_;
  std::vector<agent_event> events_;
};

class fanout_event_sink final : public event_sink {
public:
  void add_sink(std::shared_ptr<event_sink> sink) {
    if (!sink) {
      return;
    }
    std::scoped_lock lock(mutex_);
    sinks_.push_back(std::move(sink));
  }

  void publish(const agent_event& event) override {
    std::vector<std::shared_ptr<event_sink>> sinks;
    {
      std::scoped_lock lock(mutex_);
      sinks = sinks_;
    }
    std::exception_ptr first_failure;
    for (const auto& sink : sinks) {
      try {
        sink->publish(event);
      }
      catch (...) {
        if (!first_failure) first_failure = std::current_exception();
      }
    }
    if (first_failure) std::rethrow_exception(first_failure);
  }

private:
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<event_sink>> sinks_;
};

class jsonl_event_sink final : public event_sink {
public:
  explicit jsonl_event_sink(std::filesystem::path path) : path_(std::move(path)) {
  }

  void publish(const agent_event& event) override {
    std::scoped_lock lock(mutex_);
    std::ofstream output(path_, std::ios::app);
    if (!output) {
      throw std::runtime_error("failed to open agent event file: " + path_.string());
    }
    output << agent_event_to_json(event).dump() << '\n';
    output.flush();
    if (!output) {
      throw std::runtime_error("failed to write agent event file: " + path_.string());
    }
  }

private:
  std::filesystem::path path_;
  std::mutex mutex_;
};

enum class async_event_overflow_policy {
  drop_newest,
  drop_oldest,
};

struct async_event_sink_options {
  std::size_t capacity { 4096 };
  async_event_overflow_policy overflow_policy {
    async_event_overflow_policy::drop_newest
  };
  telemetry_failure_mode failure_mode { telemetry_failure_mode::ignore };
};

struct async_event_sink_stats {
  std::uint64_t published { 0 };
  std::uint64_t delivered { 0 };
  std::uint64_t dropped { 0 };
  std::uint64_t failures { 0 };
};

class async_event_sink final : public event_sink {
public:
  explicit async_event_sink(
    std::shared_ptr<event_sink> destination,
    async_event_sink_options options = {})
      : destination_(std::move(destination)), options_(options) {
    if (!destination_) {
      throw std::invalid_argument("async event sink requires a destination");
    }
    if (options_.capacity == 0) {
      throw std::invalid_argument("async event sink capacity must be positive");
    }
    worker_ = std::jthread(
      [this](std::stop_token token) { consume(token); });
  }

  ~async_event_sink() override {
    close(true);
  }

  async_event_sink(const async_event_sink&) = delete;
  async_event_sink& operator=(const async_event_sink&) = delete;

  void publish(const agent_event& event) override {
    published_.fetch_add(1, std::memory_order_relaxed);
    {
      std::scoped_lock lock(mutex_);
      if (closed_) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      if (queue_.size() >= options_.capacity) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        if (options_.overflow_policy == async_event_overflow_policy::drop_newest) {
          return;
        }
        queue_.pop_front();
      }
      queue_.push_back(event);
    }
    available_.notify_one();
  }

  void flush() {
    if (std::this_thread::get_id() == worker_.get_id()) {
      throw std::logic_error("async event sink cannot flush from its worker thread");
    }
    std::unique_lock lock(mutex_);
    drained_.wait(lock, [this] { return queue_.empty() && !delivering_; });
    if (options_.failure_mode == telemetry_failure_mode::propagate && failure_) {
      std::rethrow_exception(failure_);
    }
  }

  void close(bool drain) noexcept {
    {
      std::scoped_lock lock(mutex_);
      if (closed_) return;
      closed_ = true;
      if (!drain) {
        dropped_.fetch_add(queue_.size(), std::memory_order_relaxed);
        queue_.clear();
      }
    }
    available_.notify_all();
    if (worker_.joinable()) {
      try {
        worker_.request_stop();
        if (std::this_thread::get_id() != worker_.get_id()) {
          worker_.join();
        }
      }
      catch (...) {
      }
    }
  }

  [[nodiscard]] async_event_sink_stats stats() const noexcept {
    return {
      .published = published_.load(std::memory_order_relaxed),
      .delivered = delivered_.load(std::memory_order_relaxed),
      .dropped = dropped_.load(std::memory_order_relaxed),
      .failures = failures_.load(std::memory_order_relaxed),
    };
  }

private:
  void consume(std::stop_token token) noexcept {
    for (;;) {
      agent_event event;
      {
        std::unique_lock lock(mutex_);
        available_.wait(lock, token, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) {
          if (closed_ || token.stop_requested()) break;
          continue;
        }
        event = std::move(queue_.front());
        queue_.pop_front();
        delivering_ = true;
      }
      try {
        destination_->publish(event);
        delivered_.fetch_add(1, std::memory_order_relaxed);
      }
      catch (...) {
        failures_.fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lock(mutex_);
        if (!failure_) failure_ = std::current_exception();
      }
      {
        std::scoped_lock lock(mutex_);
        delivering_ = false;
        if (queue_.empty()) drained_.notify_all();
      }
    }
    std::scoped_lock lock(mutex_);
    delivering_ = false;
    drained_.notify_all();
  }

  std::shared_ptr<event_sink> destination_;
  async_event_sink_options options_;
  mutable std::mutex mutex_;
  std::condition_variable_any available_;
  std::condition_variable drained_;
  std::deque<agent_event> queue_;
  bool closed_ { false };
  bool delivering_ { false };
  std::exception_ptr failure_;
  std::atomic<std::uint64_t> published_ { 0 };
  std::atomic<std::uint64_t> delivered_ { 0 };
  std::atomic<std::uint64_t> dropped_ { 0 };
  std::atomic<std::uint64_t> failures_ { 0 };
  std::jthread worker_;
};

} // namespace wuwe::agent::observability

#endif // WUWE_AGENT_CORE_OBSERVABILITY_HPP
