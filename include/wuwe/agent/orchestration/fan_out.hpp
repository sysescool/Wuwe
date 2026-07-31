#ifndef WUWE_AGENT_ORCHESTRATION_FAN_OUT_HPP
#define WUWE_AGENT_ORCHESTRATION_FAN_OUT_HPP

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <wuwe/agent/orchestration/flow.hpp>

WUWE_NAMESPACE_BEGIN

enum class fan_out_failure_mode {
  collect_all,
  fail_fast,
};

enum class fan_out_item_status {
  completed,
  failed,
  cancelled,
  timed_out,
  skipped,
};

[[nodiscard]] inline std::string to_string(fan_out_item_status value) {
  switch (value) {
    case fan_out_item_status::completed:
      return "completed";
    case fan_out_item_status::failed:
      return "failed";
    case fan_out_item_status::cancelled:
      return "cancelled";
    case fan_out_item_status::timed_out:
      return "timed_out";
    case fan_out_item_status::skipped:
      return "skipped";
  }
  return "unknown";
}

enum class fan_out_stop_reason {
  none,
  cancelled,
  timed_out,
  fail_fast,
};

[[nodiscard]] inline std::string to_string(fan_out_stop_reason value) {
  switch (value) {
    case fan_out_stop_reason::none:
      return "none";
    case fan_out_stop_reason::cancelled:
      return "cancelled";
    case fan_out_stop_reason::timed_out:
      return "timed_out";
    case fan_out_stop_reason::fail_fast:
      return "fail_fast";
  }
  return "unknown";
}

struct fan_out_options {
  std::size_t max_concurrency { 4 };
  fan_out_failure_mode failure_mode { fan_out_failure_mode::collect_all };
  std::chrono::milliseconds timeout { 0 };
  std::optional<std::chrono::steady_clock::time_point> deadline;
};

struct fan_out_context {
  std::size_t index { 0 };
  std::stop_token stop_token;
  std::optional<std::chrono::steady_clock::time_point> deadline;

  [[nodiscard]] bool cancellation_requested() const noexcept {
    return stop_token.stop_requested();
  }

  [[nodiscard]] bool deadline_reached() const noexcept {
    return deadline && std::chrono::steady_clock::now() >= *deadline;
  }

  [[nodiscard]] std::chrono::milliseconds remaining_time() const noexcept {
    if (!deadline) {
      return std::chrono::milliseconds::max();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= *deadline) {
      return std::chrono::milliseconds { 0 };
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
  }
};

template<typename T>
struct fan_out_item_result {
  std::size_t index { 0 };
  fan_out_item_status status { fan_out_item_status::skipped };
  std::optional<T> value;
  std::exception_ptr exception;
  std::string error;
  std::chrono::milliseconds elapsed { 0 };
  bool detached { false };

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == fan_out_item_status::completed && value.has_value();
  }
};

template<typename T>
struct fan_out_result {
  std::vector<fan_out_item_result<T>> items;
  fan_out_stop_reason stop_reason { fan_out_stop_reason::none };
  std::chrono::milliseconds elapsed { 0 };
  std::size_t completed_count { 0 };
  std::size_t failed_count { 0 };
  std::size_t cancelled_count { 0 };
  std::size_t timed_out_count { 0 };
  std::size_t skipped_count { 0 };
  std::size_t detached_count { 0 };

  [[nodiscard]] explicit operator bool() const noexcept {
    return stop_reason == fan_out_stop_reason::none && completed_count == items.size();
  }

  [[nodiscard]] bool partial_success() const noexcept {
    return completed_count != 0 && completed_count != items.size();
  }
};

class fan_out_error : public std::runtime_error {
public:
  explicit fan_out_error(const std::string& message) : std::runtime_error(message) {
  }
};

namespace detail {

template<typename Branch, typename Input>
decltype(auto) invoke_fan_out_branch(
  const Branch& branch, const Input& input, const fan_out_context& context) {
  if constexpr (std::is_invocable_v<const Branch&, const Input&, const fan_out_context&>) {
    return std::invoke(branch, input, context);
  }
  else if constexpr (std::is_invocable_v<const Branch&, const Input&, std::stop_token>) {
    return std::invoke(branch, input, context.stop_token);
  }
  else {
    static_assert(std::is_invocable_v<const Branch&, const Input&>,
      "fan_out branches must accept (const input&), (const input&, stop_token), or "
      "(const input&, const fan_out_context&)");
    return std::invoke(branch, input);
  }
}

template<typename Branch, typename Input>
using fan_out_raw_result_t = decltype(invoke_fan_out_branch(std::declval<const Branch&>(),
  std::declval<const Input&>(), std::declval<const fan_out_context&>()));

template<typename T>
using fan_out_value_t = std::conditional_t<std::is_void_v<T>, std::monostate, std::decay_t<T>>;

template<typename Result, std::size_t I = 0, typename Tuple, typename Input>
Result invoke_fan_out_branch_at(
  std::size_t index, const Tuple& branches, const Input& input, const fan_out_context& context) {
  if constexpr (I < std::tuple_size_v<Tuple>) {
    if (index == I) {
      using branch_type = std::tuple_element_t<I, Tuple>;
      using raw_result = fan_out_raw_result_t<branch_type, Input>;
      if constexpr (std::is_void_v<raw_result>) {
        static_assert(std::is_same_v<Result, std::monostate>,
          "all fan_out branches must return the same value type");
        invoke_fan_out_branch(std::get<I>(branches), input, context);
        return {};
      }
      else {
        return invoke_fan_out_branch(std::get<I>(branches), input, context);
      }
    }
    return invoke_fan_out_branch_at<Result, I + 1>(index, branches, input, context);
  }
  else {
    throw std::out_of_range("fan_out branch index is out of range");
  }
}

template<typename T>
struct fan_out_internal_item {
  std::size_t index { 0 };
  fan_out_item_status status { fan_out_item_status::skipped };
  std::unique_ptr<T> value;
  std::exception_ptr exception;
  std::string error;
  std::chrono::milliseconds elapsed { 0 };
};

template<typename T>
struct fan_out_execution_state {
  fan_out_execution_state(std::size_t count, fan_out_options value)
      : options(std::move(value)), items(count), launched(count, false), finished(count, false) {
    for (std::size_t index = 0; index < count; ++index) {
      items[index].index = index;
    }
  }

  fan_out_options options;
  std::stop_source stop_source;
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<fan_out_internal_item<T>> items;
  std::vector<bool> launched;
  std::vector<bool> finished;
  std::size_t next_index { 0 };
  std::size_t finished_count { 0 };
  fan_out_stop_reason stop_reason { fan_out_stop_reason::none };
};

template<typename T>
void record_fan_out_item(const std::shared_ptr<fan_out_execution_state<T>>& state,
  fan_out_internal_item<T> item) noexcept {
  bool request_stop = false;
  {
    std::scoped_lock lock(state->mutex);
    const auto index = item.index;
    if (state->finished[index]) {
      return;
    }
    const auto failed = item.status == fan_out_item_status::failed;
    const auto timed_out = item.status == fan_out_item_status::timed_out;
    state->items[index] = std::move(item);
    state->finished[index] = true;
    ++state->finished_count;
    if (timed_out && state->stop_reason == fan_out_stop_reason::none) {
      state->stop_reason = fan_out_stop_reason::timed_out;
      request_stop = true;
    }
    else if (failed && state->options.failure_mode == fan_out_failure_mode::fail_fast &&
             state->stop_reason == fan_out_stop_reason::none) {
      state->stop_reason = fan_out_stop_reason::fail_fast;
      request_stop = true;
    }
  }
  if (request_stop) {
    state->stop_source.request_stop();
  }
  state->condition.notify_all();
}

template<typename T, typename Invoker>
void execute_fan_out_item(const std::shared_ptr<fan_out_execution_state<T>>& state,
  const std::shared_ptr<const Invoker>& invoker, std::size_t index,
  const std::optional<std::chrono::steady_clock::time_point>& deadline) noexcept {
  const auto started = std::chrono::steady_clock::now();
  fan_out_internal_item<T> item { .index = index };
  try {
    fan_out_context context {
      .index = index,
      .stop_token = state->stop_source.get_token(),
      .deadline = deadline,
    };
    if (context.cancellation_requested()) {
      item.status = fan_out_item_status::cancelled;
      item.error = "fan_out branch cancelled before execution";
    }
    else {
      item.value = std::make_unique<T>(std::invoke(*invoker, index, context));
      item.status = fan_out_item_status::completed;
    }
  }
  catch (const std::exception& ex) {
    item.status = fan_out_item_status::failed;
    item.exception = std::current_exception();
    item.error = ex.what();
  }
  catch (...) {
    item.status = fan_out_item_status::failed;
    item.exception = std::current_exception();
    item.error = "fan_out branch failed with an unknown exception";
  }
  const auto finished_at = std::chrono::steady_clock::now();
  item.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(finished_at - started);
  if (deadline && finished_at >= *deadline) {
    item.status = fan_out_item_status::timed_out;
    item.value.reset();
    if (item.error.empty()) {
      item.error = "fan_out branch exceeded the operation timeout";
    }
  }
  record_fan_out_item(state, std::move(item));
}

template<typename T>
fan_out_item_result<T> interrupted_fan_out_item(
  std::size_t index, fan_out_stop_reason reason, bool detached) {
  fan_out_item_result<T> item {
    .index = index,
    .status = reason == fan_out_stop_reason::timed_out ? fan_out_item_status::timed_out
                                                       : fan_out_item_status::cancelled,
    .detached = detached,
  };
  if (reason == fan_out_stop_reason::timed_out) {
    item.error = detached ? "fan_out branch exceeded the operation timeout and remains detached"
                          : "fan_out branch exceeded the operation timeout";
  }
  else if (reason == fan_out_stop_reason::fail_fast) {
    item.error = detached ? "fan_out branch cancelled after fail-fast and remains detached"
                          : "fan_out branch cancelled after fail-fast";
  }
  else {
    item.error =
      detached ? "fan_out branch cancelled and remains detached" : "fan_out branch cancelled";
  }
  return item;
}

template<typename T, typename Invoker>
void run_fan_out_worker(const std::shared_ptr<fan_out_execution_state<T>>& state,
  const std::shared_ptr<const Invoker>& invoker,
  const std::optional<std::chrono::steady_clock::time_point>& deadline) noexcept {
  for (;;) {
    std::size_t index = 0;
    bool request_timeout_stop = false;
    {
      std::scoped_lock lock(state->mutex);
      if (state->stop_reason != fan_out_stop_reason::none ||
          state->next_index >= state->items.size()) {
        return;
      }
      if (deadline && std::chrono::steady_clock::now() >= *deadline) {
        state->stop_reason = fan_out_stop_reason::timed_out;
        request_timeout_stop = true;
      }
      else {
        index = state->next_index++;
        state->launched[index] = true;
      }
    }
    if (request_timeout_stop) {
      state->stop_source.request_stop();
      state->condition.notify_all();
      return;
    }
    execute_fan_out_item(state, invoker, index, deadline);
  }
}

template<typename T, typename Invoker>
fan_out_result<T> run_fan_out(std::size_t count, fan_out_options options,
  const std::shared_ptr<const Invoker>& invoker, std::stop_token external_stop_token) {
  if (options.max_concurrency == 0) {
    throw std::invalid_argument("fan_out max_concurrency must be greater than zero");
  }
  if (options.timeout.count() < 0) {
    throw std::invalid_argument("fan_out timeout must not be negative");
  }

  const auto started = std::chrono::steady_clock::now();
  auto deadline = options.deadline;
  if (options.timeout.count() > 0) {
    const auto relative_deadline = started + options.timeout;
    if (!deadline || relative_deadline < *deadline) {
      deadline = relative_deadline;
    }
  }
  auto state = std::make_shared<fan_out_execution_state<T>>(count, std::move(options));
  std::optional<std::stop_callback<std::function<void()>>> external_stop;
  if (external_stop_token.stop_possible()) {
    external_stop.emplace(external_stop_token, std::function<void()>([state] {
      bool request_stop = false;
      {
        std::scoped_lock lock(state->mutex);
        if (state->stop_reason == fan_out_stop_reason::none &&
            state->finished_count != state->items.size()) {
          state->stop_reason = fan_out_stop_reason::cancelled;
          request_stop = true;
        }
      }
      if (request_stop) {
        state->stop_source.request_stop();
      }
      state->condition.notify_all();
    }));
  }

  std::size_t workers_started = 0;
  std::exception_ptr worker_start_exception;
  std::string worker_start_error;
  const auto worker_count = (std::min)(state->options.max_concurrency, count);
  for (std::size_t index = 0; index < worker_count; ++index) {
    try {
      std::thread worker(
        [state, invoker, deadline] { run_fan_out_worker(state, invoker, deadline); });
      try {
        worker.detach();
      }
      catch (...) {
        if (worker.joinable())
          worker.join();
      }
      ++workers_started;
    }
    catch (const std::exception& ex) {
      worker_start_exception = std::current_exception();
      worker_start_error = ex.what();
    }
    catch (...) {
      worker_start_exception = std::current_exception();
      worker_start_error = "unknown thread creation error";
    }
  }
  if (count != 0 && workers_started == 0) {
    std::scoped_lock lock(state->mutex);
    for (std::size_t index = 0; index < count; ++index) {
      state->items[index] = {
        .index = index,
        .status = fan_out_item_status::failed,
        .exception = worker_start_exception,
        .error = "failed to start fan_out workers: " + worker_start_error,
      };
      state->finished[index] = true;
    }
    state->finished_count = count;
  }

  bool request_timeout_stop = false;
  {
    std::unique_lock lock(state->mutex);
    const auto finished = [&] {
      return state->finished_count == count || (state->stop_reason != fan_out_stop_reason::none &&
                                                 state->stop_source.stop_requested());
    };
    if (deadline) {
      if (!state->condition.wait_until(lock, *deadline, finished) &&
          state->finished_count != count && state->stop_reason == fan_out_stop_reason::none) {
        state->stop_reason = fan_out_stop_reason::timed_out;
        request_timeout_stop = true;
      }
    }
    else {
      state->condition.wait(lock, finished);
    }
  }
  if (request_timeout_stop) {
    state->stop_source.request_stop();
    state->condition.notify_all();
  }

  fan_out_result<T> output;
  {
    std::scoped_lock lock(state->mutex);
    output.stop_reason = state->stop_reason;
    output.items.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      if (state->finished[index]) {
        auto& stored = state->items[index];
        fan_out_item_result<T> item {
          .index = stored.index,
          .status = stored.status,
          .exception = std::move(stored.exception),
          .error = std::move(stored.error),
          .elapsed = stored.elapsed,
        };
        if (stored.value) {
          try {
            item.value.emplace(std::move(*stored.value));
          }
          catch (const std::exception& ex) {
            item.status = fan_out_item_status::failed;
            item.exception = std::current_exception();
            item.error = std::string("failed to transfer fan_out result: ") + ex.what();
          }
          catch (...) {
            item.status = fan_out_item_status::failed;
            item.exception = std::current_exception();
            item.error = "failed to transfer fan_out result";
          }
        }
        output.items.push_back(std::move(item));
      }
      else if (state->launched[index]) {
        output.items.push_back(interrupted_fan_out_item<T>(index, state->stop_reason, true));
      }
      else {
        output.items.push_back({
          .index = index,
          .status = fan_out_item_status::skipped,
          .error = "fan_out branch was not started after " + to_string(state->stop_reason),
        });
      }
    }
  }
  output.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - started);
  for (const auto& item : output.items) {
    switch (item.status) {
      case fan_out_item_status::completed:
        ++output.completed_count;
        break;
      case fan_out_item_status::failed:
        ++output.failed_count;
        break;
      case fan_out_item_status::cancelled:
        ++output.cancelled_count;
        break;
      case fan_out_item_status::timed_out:
        ++output.timed_out_count;
        break;
      case fan_out_item_status::skipped:
        ++output.skipped_count;
        break;
    }
    if (item.detached) {
      ++output.detached_count;
    }
  }
  return output;
}

} // namespace detail

template<typename... Branches>
class fan_out_step {
public:
  fan_out_step(fan_out_options options, Branches... branches)
      : options_(std::move(options)),
        branches_(std::make_shared<const std::tuple<Branches...>>(std::move(branches)...)) {
    static_assert(sizeof...(Branches) != 0, "fan_out requires at least one branch");
    if (options_.max_concurrency == 0) {
      throw std::invalid_argument("fan_out max_concurrency must be greater than zero");
    }
    if (options_.timeout.count() < 0) {
      throw std::invalid_argument("fan_out timeout must not be negative");
    }
  }

  template<typename T>
  auto operator()(T&& input) const {
    return run(std::forward<T>(input));
  }

  template<typename T>
  auto operator()(T&& input, const flow_context& context) const {
    return run(std::forward<T>(input), context.stop_token);
  }

  template<typename T>
  auto run(T&& input, std::stop_token stop_token = {}) const {
    using input_type = std::decay_t<T>;
    using tuple_type = std::tuple<Branches...>;
    using first_branch = std::tuple_element_t<0, tuple_type>;
    using result_type =
      detail::fan_out_value_t<detail::fan_out_raw_result_t<first_branch, input_type>>;
    static_assert((std::is_same_v<result_type,
                     detail::fan_out_value_t<detail::fan_out_raw_result_t<Branches, input_type>>> &&
                    ...),
      "all fan_out branches must return the same value type");
    static_assert(std::is_move_constructible_v<result_type>,
      "fan_out result values must be move constructible");

    auto stable_input = std::make_shared<const input_type>(std::forward<T>(input));
    using invoker_type = std::function<result_type(std::size_t, const fan_out_context&)>;
    auto invoker =
      std::make_shared<const invoker_type>([branches = branches_, stable_input](std::size_t index,
                                             const fan_out_context& context) -> result_type {
        return detail::invoke_fan_out_branch_at<result_type>(
          index, *branches, *stable_input, context);
      });
    return detail::run_fan_out<result_type>(sizeof...(Branches), options_, invoker, stop_token);
  }

private:
  fan_out_options options_;
  std::shared_ptr<const std::tuple<Branches...>> branches_;
};

template<typename... Branches>
auto fan_out(fan_out_options options, Branches&&... branches) {
  static_assert(sizeof...(Branches) != 0, "fan_out requires at least one branch");
  return fan_out_step<std::decay_t<Branches>...>(
    std::move(options), std::forward<Branches>(branches)...);
}

template<typename First, typename... Rest>
  requires(!std::is_same_v<std::decay_t<First>, fan_out_options>)
auto fan_out(First&& first, Rest&&... rest) {
  return fan_out(fan_out_options {}, std::forward<First>(first), std::forward<Rest>(rest)...);
}

template<typename Worker>
class fan_out_each_step {
public:
  fan_out_each_step(fan_out_options options, Worker worker)
      : options_(std::move(options)), worker_(std::make_shared<const Worker>(std::move(worker))) {
    if (options_.max_concurrency == 0) {
      throw std::invalid_argument("fan_out_each max_concurrency must be greater than zero");
    }
    if (options_.timeout.count() < 0) {
      throw std::invalid_argument("fan_out_each timeout must not be negative");
    }
  }

  template<typename Range>
  auto operator()(Range&& input) const {
    return run(std::forward<Range>(input));
  }

  template<typename Range>
  auto operator()(Range&& input, const flow_context& context) const {
    return run(std::forward<Range>(input), context.stop_token);
  }

  template<typename Range>
  auto run(Range&& input, std::stop_token stop_token = {}) const {
    using range_type = std::decay_t<Range>;
    static_assert(
      requires(const range_type& value, std::size_t index) {
        value.size();
        value[index];
      }, "fan_out_each input must be a sized random-access range");
    using item_type =
      std::remove_cvref_t<decltype(std::declval<const range_type&>()[std::declval<std::size_t>()])>;
    using result_type = detail::fan_out_value_t<detail::fan_out_raw_result_t<Worker, item_type>>;
    static_assert(std::is_move_constructible_v<result_type>,
      "fan_out_each result values must be move constructible");

    auto stable_input = std::make_shared<const range_type>(std::forward<Range>(input));
    using invoker_type = std::function<result_type(std::size_t, const fan_out_context&)>;
    auto invoker =
      std::make_shared<const invoker_type>([worker = worker_, stable_input](std::size_t index,
                                             const fan_out_context& context) -> result_type {
        using raw_result = detail::fan_out_raw_result_t<Worker, item_type>;
        if constexpr (std::is_void_v<raw_result>) {
          detail::invoke_fan_out_branch(*worker, (*stable_input)[index], context);
          return {};
        }
        else {
          return detail::invoke_fan_out_branch(*worker, (*stable_input)[index], context);
        }
      });
    return detail::run_fan_out<result_type>(stable_input->size(), options_, invoker, stop_token);
  }

private:
  fan_out_options options_;
  std::shared_ptr<const Worker> worker_;
};

template<typename Worker>
auto fan_out_each(fan_out_options options, Worker&& worker) {
  return fan_out_each_step<std::decay_t<Worker>>(std::move(options), std::forward<Worker>(worker));
}

template<typename Worker>
auto fan_out_each(Worker&& worker) {
  return fan_out_each(fan_out_options {}, std::forward<Worker>(worker));
}

template<typename Reducer>
class fan_in_step {
public:
  explicit fan_in_step(Reducer reducer) : reducer_(std::move(reducer)) {
  }

  template<typename T>
  decltype(auto) operator()(T&& value) const {
    return std::invoke(reducer_, std::forward<T>(value));
  }

private:
  Reducer reducer_;
};

template<typename Reducer>
auto fan_in(Reducer&& reducer) {
  return fan_in_step<std::decay_t<Reducer>>(std::forward<Reducer>(reducer));
}

class fan_in_all_step {
public:
  template<typename T>
  std::vector<T> operator()(fan_out_result<T> result) const {
    if (!result) {
      throw fan_out_error(
        "fan_in_all requires every fan_out branch to complete successfully; stop_reason=" +
        to_string(result.stop_reason) + ", completed=" + std::to_string(result.completed_count) +
        ", failed=" + std::to_string(result.failed_count) +
        ", cancelled=" + std::to_string(result.cancelled_count) +
        ", timed_out=" + std::to_string(result.timed_out_count) +
        ", skipped=" + std::to_string(result.skipped_count));
    }
    std::vector<T> output;
    output.reserve(result.items.size());
    for (auto& item : result.items) {
      output.push_back(std::move(*item.value));
    }
    return output;
  }
};

inline fan_in_all_step fan_in_all() {
  return {};
}

class fan_in_successes_step {
public:
  template<typename T>
  std::vector<T> operator()(fan_out_result<T> result) const {
    std::vector<T> output;
    output.reserve(result.completed_count);
    for (auto& item : result.items) {
      if (item) {
        output.push_back(std::move(*item.value));
      }
    }
    return output;
  }
};

inline fan_in_successes_step fan_in_successes() {
  return {};
}

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_ORCHESTRATION_FAN_OUT_HPP
