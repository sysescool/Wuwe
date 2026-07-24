#ifndef WUWE_AGENT_REASONING_BEST_OF_N_HPP
#define WUWE_AGENT_REASONING_BEST_OF_N_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/orchestration/fan_out.hpp>
#include <wuwe/agent/core/observability.hpp>
#include <wuwe/agent/reasoning/reasoning_runner.hpp>

namespace wuwe::agent::reasoning {

enum class best_of_n_candidate_status {
  eligible,
  rejected,
  generation_failed,
  scoring_failed,
  budget_exceeded,
  side_effect_blocked,
  cancelled,
  timed_out,
  skipped,
};

[[nodiscard]] inline std::string to_string(best_of_n_candidate_status value) {
  switch (value) {
    case best_of_n_candidate_status::eligible: return "eligible";
    case best_of_n_candidate_status::rejected: return "rejected";
    case best_of_n_candidate_status::generation_failed: return "generation_failed";
    case best_of_n_candidate_status::scoring_failed: return "scoring_failed";
    case best_of_n_candidate_status::budget_exceeded: return "budget_exceeded";
    case best_of_n_candidate_status::side_effect_blocked:
      return "side_effect_blocked";
    case best_of_n_candidate_status::cancelled: return "cancelled";
    case best_of_n_candidate_status::timed_out: return "timed_out";
    case best_of_n_candidate_status::skipped: return "skipped";
  }
  return "unknown";
}

enum class best_of_n_stop_reason {
  none,
  cancelled,
  timed_out,
  budget_exceeded,
  no_eligible_candidate,
  selection_failed,
};

[[nodiscard]] inline std::string to_string(best_of_n_stop_reason value) {
  switch (value) {
    case best_of_n_stop_reason::none: return "none";
    case best_of_n_stop_reason::cancelled: return "cancelled";
    case best_of_n_stop_reason::timed_out: return "timed_out";
    case best_of_n_stop_reason::budget_exceeded: return "budget_exceeded";
    case best_of_n_stop_reason::no_eligible_candidate:
      return "no_eligible_candidate";
    case best_of_n_stop_reason::selection_failed: return "selection_failed";
  }
  return "unknown";
}

enum class best_of_n_event_type {
  started,
  candidate_started,
  candidate_generated,
  candidate_scored,
  candidate_rejected,
  candidate_failed,
  candidate_budget_exceeded,
  candidate_cancelled,
  candidate_timed_out,
  candidate_skipped,
  selected,
  completed,
  failed,
  cancelled,
};

enum class best_of_n_side_effect_policy {
  isolate,
  allow,
};

[[nodiscard]] inline std::string to_string(best_of_n_side_effect_policy value) {
  switch (value) {
    case best_of_n_side_effect_policy::isolate: return "isolate";
    case best_of_n_side_effect_policy::allow: return "allow";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(best_of_n_event_type value) {
  switch (value) {
    case best_of_n_event_type::started: return "started";
    case best_of_n_event_type::candidate_started: return "candidate_started";
    case best_of_n_event_type::candidate_generated: return "candidate_generated";
    case best_of_n_event_type::candidate_scored: return "candidate_scored";
    case best_of_n_event_type::candidate_rejected: return "candidate_rejected";
    case best_of_n_event_type::candidate_failed: return "candidate_failed";
    case best_of_n_event_type::candidate_budget_exceeded:
      return "candidate_budget_exceeded";
    case best_of_n_event_type::candidate_cancelled: return "candidate_cancelled";
    case best_of_n_event_type::candidate_timed_out: return "candidate_timed_out";
    case best_of_n_event_type::candidate_skipped: return "candidate_skipped";
    case best_of_n_event_type::selected: return "selected";
    case best_of_n_event_type::completed: return "completed";
    case best_of_n_event_type::failed: return "failed";
    case best_of_n_event_type::cancelled: return "cancelled";
  }
  return "unknown";
}

struct best_of_n_context {
  std::size_t index { 0 };
  std::stop_token stop_token;
  std::optional<std::chrono::steady_clock::time_point> deadline;
  best_of_n_side_effect_policy side_effects {
    best_of_n_side_effect_policy::isolate
  };

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

struct best_of_n_score {
  double value { 0.0 };
  bool accepted { true };
  std::string rationale;
  std::map<std::string, std::string> metadata;
  reasoning_usage usage;
};

struct best_of_n_candidate {
  std::size_t index { 0 };
  best_of_n_candidate_status status { best_of_n_candidate_status::skipped };
  reasoning_request request;
  reasoning_result result;
  std::optional<best_of_n_score> score;
  std::string error;
  std::chrono::milliseconds elapsed { 0 };
  bool detached { false };

  [[nodiscard]] bool eligible() const noexcept {
    return status == best_of_n_candidate_status::eligible && score.has_value();
  }
};

struct best_of_n_event {
  std::size_t sequence { 0 };
  best_of_n_event_type type { best_of_n_event_type::started };
  std::optional<std::size_t> candidate_index;
  std::optional<double> score;
  std::string message;
  std::chrono::milliseconds elapsed { 0 };
  std::map<std::string, std::string> metadata;
};

using best_of_n_observer = std::function<void(const best_of_n_event&)>;
using best_of_n_candidate_generator = std::function<reasoning_result(
  const reasoning_request&, const best_of_n_context&)>;
using best_of_n_candidate_scorer = std::function<best_of_n_score(
  const reasoning_request&,
  const reasoning_result&,
  const best_of_n_context&)>;
using best_of_n_request_builder = std::function<reasoning_request(
  const reasoning_request&, std::size_t)>;
using best_of_n_contextual_request_builder = std::function<reasoning_request(
  const reasoning_request&, std::size_t, const best_of_n_context&)>;
using best_of_n_selector = std::function<std::optional<std::size_t>(
  const std::vector<best_of_n_candidate>&)>;
using best_of_n_contextual_selector = std::function<std::optional<std::size_t>(
  const std::vector<best_of_n_candidate>&, const best_of_n_context&)>;
using best_of_n_vote_key = std::function<std::string(const best_of_n_candidate&)>;

struct best_of_n_budget {
  std::size_t max_model_calls { 0 };
  std::size_t max_total_tokens { 0 };
  double max_cost_usd { 0.0 };
  std::size_t estimated_model_calls_per_candidate { 1 };
  std::size_t estimated_total_tokens_per_candidate { 0 };
  double estimated_cost_usd_per_candidate { 0.0 };
  std::size_t estimated_scorer_model_calls_per_candidate { 0 };
  std::size_t estimated_scorer_total_tokens_per_candidate { 0 };
  double estimated_scorer_cost_usd_per_candidate { 0.0 };
};

struct best_of_n_options {
  std::size_t candidate_count { 3 };
  std::size_t max_concurrency { 3 };
  std::chrono::milliseconds timeout { 0 };
  std::optional<double> minimum_score;
  double score_tie_tolerance { 1e-9 };
  bool prefer_lower_cost_on_tie { true };
  best_of_n_side_effect_policy side_effects {
    best_of_n_side_effect_policy::isolate
  };
  best_of_n_budget budget;
};

struct best_of_n_runner_options {
  best_of_n_candidate_generator generator;
  best_of_n_candidate_scorer scorer;
  best_of_n_request_builder request_builder;
  best_of_n_contextual_request_builder contextual_request_builder;
  best_of_n_selector selector;
  best_of_n_contextual_selector contextual_selector;
  best_of_n_observer observer;
  observability::telemetry_failure_mode telemetry_failure_mode {
    observability::telemetry_failure_mode::ignore
  };
};

struct best_of_n_run_options {
  best_of_n_options policy;
  std::stop_token stop_token;
};

struct best_of_n_result {
  bool completed { false };
  std::optional<std::size_t> selected_index;
  std::vector<best_of_n_candidate> candidates;
  // Usage reported by candidate results returned to the coordinator.
  reasoning_usage aggregate_usage;
  // Budget-side snapshot including completed scorer/generator usage and
  // reservations still held by detached work at the time run() returns.
  reasoning_usage budget_accounted_usage;
  reasoning_usage outstanding_reserved_usage;
  best_of_n_stop_reason stop_reason { best_of_n_stop_reason::none };
  std::string error;
  std::chrono::milliseconds elapsed { 0 };
  std::size_t eligible_count { 0 };
  std::size_t rejected_count { 0 };
  std::size_t failed_count { 0 };
  std::size_t budget_exceeded_count { 0 };
  std::size_t side_effect_blocked_count { 0 };
  std::size_t cancelled_count { 0 };
  std::size_t timed_out_count { 0 };
  std::size_t skipped_count { 0 };
  std::size_t detached_count { 0 };
  std::size_t coordination_detached_count { 0 };
  std::size_t telemetry_error_count { 0 };
  std::vector<best_of_n_event> trace;

  [[nodiscard]] explicit operator bool() const noexcept {
    return completed && selected_index.has_value();
  }

  [[nodiscard]] const best_of_n_candidate* selected_candidate() const noexcept {
    if (!selected_index || *selected_index >= candidates.size()) {
      return nullptr;
    }
    const auto& candidate = candidates[*selected_index];
    return candidate.index == *selected_index ? &candidate : nullptr;
  }
};

class best_of_n_run {
public:
  best_of_n_run() = default;

  best_of_n_run(std::jthread worker, std::future<best_of_n_result> future)
      : worker_(std::move(worker)), future_(std::move(future)) {
  }

  best_of_n_run(const best_of_n_run&) = delete;
  best_of_n_run& operator=(const best_of_n_run&) = delete;
  best_of_n_run(best_of_n_run&&) noexcept = default;
  best_of_n_run& operator=(best_of_n_run&&) noexcept = default;

  [[nodiscard]] bool valid() const noexcept {
    return future_.valid();
  }

  void request_stop() {
    worker_.request_stop();
  }

  [[nodiscard]] bool stop_requested() const noexcept {
    return worker_.get_stop_token().stop_requested();
  }

  void wait() const {
    future_.wait();
  }

  best_of_n_result get() {
    return future_.get();
  }

private:
  std::jthread worker_;
  std::future<best_of_n_result> future_;
};

namespace detail {

inline void merge_reasoning_usage(reasoning_usage& target, const reasoning_usage& value) {
  const auto add = [](std::size_t left, std::size_t right) noexcept {
    const auto maximum = (std::numeric_limits<std::size_t>::max)();
    return right > maximum - left ? maximum : left + right;
  };
  target.model_calls = add(target.model_calls, value.model_calls);
  target.tool_calls = add(target.tool_calls, value.tool_calls);
  target.tool_rounds = add(target.tool_rounds, value.tool_rounds);
  target.max_tool_rounds = (std::max)(target.max_tool_rounds, value.max_tool_rounds);
  target.reflection_calls = add(target.reflection_calls, value.reflection_calls);
  target.plan_steps = add(target.plan_steps, value.plan_steps);
  target.prompt_tokens = add(target.prompt_tokens, value.prompt_tokens);
  target.completion_tokens = add(target.completion_tokens, value.completion_tokens);
  target.total_tokens = add(target.total_tokens, value.total_tokens);
  const auto add_cost = [](double left, double right) noexcept {
    const auto maximum = (std::numeric_limits<double>::max)();
    if (!std::isfinite(left) || left < 0.0 ||
        !std::isfinite(right) || right < 0.0 || right > maximum - left) {
      return maximum;
    }
    return left + right;
  };
  target.estimated_cost_usd =
    add_cost(target.estimated_cost_usd, value.estimated_cost_usd);
  target.cost_usd = add_cost(target.cost_usd, value.cost_usd);
  target.estimated_token_calls =
    add(target.estimated_token_calls, value.estimated_token_calls);
}

struct best_of_n_trace_state {
  explicit best_of_n_trace_state(
    best_of_n_observer value,
    observability::telemetry_failure_mode failure_mode)
      : observer(std::move(value)),
        telemetry_failure_mode(failure_mode),
        started(std::chrono::steady_clock::now()) {
  }

  std::mutex mutex;
  std::mutex observer_mutex;
  std::size_t next_sequence { 0 };
  std::vector<best_of_n_event> trace;
  best_of_n_observer observer;
  observability::telemetry_failure_mode telemetry_failure_mode;
  std::atomic<std::size_t> telemetry_error_count { 0 };
  std::chrono::steady_clock::time_point started;
  std::atomic<bool> accept_worker_events { true };
};

struct best_of_n_budget_reservation {
  std::size_t model_calls { 0 };
  std::size_t total_tokens { 0 };
  double cost_usd { 0.0 };
  bool active { false };
};

struct best_of_n_budget_snapshot {
  reasoning_usage accounted_usage;
  reasoning_usage outstanding_reserved_usage;
};

class best_of_n_budget_state {
public:
  explicit best_of_n_budget_state(best_of_n_budget value)
      : budget_(std::move(value)) {
  }

  [[nodiscard]] std::optional<best_of_n_budget_reservation>
  try_reserve_candidate() {
    return try_reserve({
      .model_calls = budget_.estimated_model_calls_per_candidate,
      .total_tokens = budget_.estimated_total_tokens_per_candidate,
      .cost_usd = budget_.estimated_cost_usd_per_candidate,
      .active = true,
    });
  }

  [[nodiscard]] std::optional<best_of_n_budget_reservation>
  try_reserve_scorer() {
    return try_reserve({
      .model_calls = budget_.estimated_scorer_model_calls_per_candidate,
      .total_tokens = budget_.estimated_scorer_total_tokens_per_candidate,
      .cost_usd = budget_.estimated_scorer_cost_usd_per_candidate,
      .active = true,
    });
  }

  [[nodiscard]] bool complete(
    const best_of_n_budget_reservation& reservation,
    const reasoning_usage& usage) {
    std::scoped_lock lock(mutex_);
    release(reservation);
    if (!valid_usage(usage)) {
      exceeded_ = true;
      if (error_.empty()) {
        error_ = "candidate reported invalid aggregate usage";
      }
      return false;
    }
    if (!checked_add(actual_model_calls_, usage.model_calls) ||
        !checked_add(actual_total_tokens_, usage.total_tokens) ||
        !checked_add(actual_cost_usd_, usage.cost_usd)) {
      exceeded_ = true;
      if (error_.empty()) {
        error_ = "best-of-n aggregate usage overflowed its accounting range";
      }
      return false;
    }
    const auto exceeded =
      exceeds(actual_model_calls_, budget_.max_model_calls) ||
      exceeds(actual_total_tokens_, budget_.max_total_tokens) ||
      exceeds_cost(actual_cost_usd_, budget_.max_cost_usd);
    if (exceeded) {
      exceeded_ = true;
      if (error_.empty()) {
        error_ = "best-of-n aggregate budget was exceeded by candidate usage";
      }
    }
    return !exceeded;
  }

  [[nodiscard]] bool exceeded() const {
    std::scoped_lock lock(mutex_);
    return exceeded_;
  }

  [[nodiscard]] std::string error() const {
    std::scoped_lock lock(mutex_);
    return error_;
  }

  [[nodiscard]] best_of_n_budget_snapshot snapshot() const {
    std::scoped_lock lock(mutex_);
    reasoning_usage outstanding {
      .model_calls = reserved_model_calls_,
      .total_tokens = reserved_total_tokens_,
      .estimated_cost_usd = reserved_cost_usd_,
      .cost_usd = reserved_cost_usd_,
    };
    reasoning_usage accounted {
      .model_calls = actual_model_calls_,
      .total_tokens = actual_total_tokens_,
      .cost_usd = actual_cost_usd_,
    };
    merge_reasoning_usage(accounted, outstanding);
    return {
      .accounted_usage = std::move(accounted),
      .outstanding_reserved_usage = std::move(outstanding),
    };
  }

private:
  [[nodiscard]] std::optional<best_of_n_budget_reservation> try_reserve(
    best_of_n_budget_reservation reservation) {
    std::scoped_lock lock(mutex_);
    if (would_exceed(
          actual_model_calls_, reserved_model_calls_, reservation.model_calls,
          budget_.max_model_calls) ||
        would_exceed(
          actual_total_tokens_, reserved_total_tokens_, reservation.total_tokens,
          budget_.max_total_tokens) ||
        would_exceed_cost(
          actual_cost_usd_, reserved_cost_usd_, reservation.cost_usd,
          budget_.max_cost_usd) ||
        !can_add(reserved_model_calls_, reservation.model_calls) ||
        !can_add(reserved_total_tokens_, reservation.total_tokens) ||
        !can_add(reserved_cost_usd_, reservation.cost_usd)) {
      exceeded_ = true;
      if (error_.empty()) {
        error_ = "best-of-n aggregate budget has no capacity for the next operation";
      }
      return std::nullopt;
    }
    reserved_model_calls_ += reservation.model_calls;
    reserved_total_tokens_ += reservation.total_tokens;
    reserved_cost_usd_ += reservation.cost_usd;
    return reservation;
  }

  void release(const best_of_n_budget_reservation& reservation) noexcept {
    if (!reservation.active) {
      return;
    }
    reserved_model_calls_ -= reservation.model_calls;
    reserved_total_tokens_ -= reservation.total_tokens;
    reserved_cost_usd_ -= reservation.cost_usd;
  }

  static bool valid_usage(const reasoning_usage& usage) noexcept {
    return std::isfinite(usage.cost_usd) && usage.cost_usd >= 0.0 &&
           std::isfinite(usage.estimated_cost_usd) &&
           usage.estimated_cost_usd >= 0.0;
  }

  static bool checked_add(std::size_t& target, std::size_t value) noexcept {
    const auto maximum = (std::numeric_limits<std::size_t>::max)();
    if (value > maximum - target) {
      target = maximum;
      return false;
    }
    target += value;
    return true;
  }

  static bool can_add(std::size_t target, std::size_t value) noexcept {
    return value <= (std::numeric_limits<std::size_t>::max)() - target;
  }

  static bool can_add(double target, double value) noexcept {
    return value <= (std::numeric_limits<double>::max)() - target;
  }

  static bool checked_add(double& target, double value) noexcept {
    if (value > (std::numeric_limits<double>::max)() - target) {
      target = (std::numeric_limits<double>::max)();
      return false;
    }
    target += value;
    return std::isfinite(target);
  }

  static bool would_exceed(
    std::size_t actual,
    std::size_t reserved,
    std::size_t requested,
    std::size_t maximum) noexcept {
    return maximum != 0 &&
           (actual > maximum || reserved > maximum - actual ||
            requested > maximum - actual - reserved);
  }

  static bool exceeds(std::size_t actual, std::size_t maximum) noexcept {
    return maximum != 0 && actual > maximum;
  }

  static bool would_exceed_cost(
    double actual,
    double reserved,
    double requested,
    double maximum) noexcept {
    return maximum > 0.0 && actual + reserved + requested > maximum + 1e-12;
  }

  static bool exceeds_cost(double actual, double maximum) noexcept {
    return maximum > 0.0 && actual > maximum + 1e-12;
  }

  mutable std::mutex mutex_;
  best_of_n_budget budget_;
  std::size_t reserved_model_calls_ { 0 };
  std::size_t reserved_total_tokens_ { 0 };
  double reserved_cost_usd_ { 0.0 };
  std::size_t actual_model_calls_ { 0 };
  std::size_t actual_total_tokens_ { 0 };
  double actual_cost_usd_ { 0.0 };
  bool exceeded_ { false };
  std::string error_;
};

inline void emit_best_of_n_event(
  const std::shared_ptr<best_of_n_trace_state>& state,
  best_of_n_event event,
  bool coordinator_event = false) {
  if (!coordinator_event && !state->accept_worker_events.load()) {
    return;
  }
  event.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - state->started);
  {
    std::scoped_lock lock(state->mutex);
    if (!coordinator_event && !state->accept_worker_events.load()) {
      return;
    }
    event.sequence = state->next_sequence++;
    state->trace.push_back(event);
  }
  if (state->observer) {
    std::scoped_lock lock(state->observer_mutex);
    if (!coordinator_event && !state->accept_worker_events.load()) {
      return;
    }
    if (!observability::invoke_telemetry(
          state->telemetry_failure_mode,
          [&] { state->observer(event); })) {
      state->telemetry_error_count.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

inline bool lower_cost_candidate(
  const best_of_n_candidate& candidate,
  const best_of_n_candidate& incumbent) noexcept {
  constexpr double tolerance = 1e-12;
  const auto candidate_cost = candidate.result.usage.cost_usd;
  const auto incumbent_cost = incumbent.result.usage.cost_usd;
  if (candidate_cost + tolerance < incumbent_cost) {
    return true;
  }
  if (incumbent_cost + tolerance < candidate_cost) {
    return false;
  }
  if (candidate.result.usage.total_tokens != incumbent.result.usage.total_tokens) {
    return candidate.result.usage.total_tokens < incumbent.result.usage.total_tokens;
  }
  return candidate.index < incumbent.index;
}

} // namespace detail

class best_of_n_runner {
public:
  explicit best_of_n_runner(best_of_n_runner_options options)
      : options_(std::move(options)) {
    if (!options_.generator) {
      throw std::invalid_argument("best_of_n_runner requires a candidate generator");
    }
    if (!options_.scorer) {
      throw std::invalid_argument("best_of_n_runner requires a candidate scorer");
    }
    if (options_.request_builder && options_.contextual_request_builder) {
      throw std::invalid_argument(
        "best_of_n_runner accepts either request_builder or contextual_request_builder");
    }
    if (options_.selector && options_.contextual_selector) {
      throw std::invalid_argument(
        "best_of_n_runner accepts either selector or contextual_selector");
    }
  }

  [[nodiscard]] best_of_n_result run(
    reasoning_request request,
    best_of_n_run_options run_options = {}) const {
    validate(run_options.policy);
    const auto started = std::chrono::steady_clock::now();
    auto execution_options =
      std::make_shared<const best_of_n_runner_options>(options_);
    auto trace_state = std::make_shared<detail::best_of_n_trace_state>(
      execution_options->observer,
      execution_options->telemetry_failure_mode);
    detail::emit_best_of_n_event(
      trace_state,
      { .type = best_of_n_event_type::started,
        .message = "best-of-n reasoning started" },
      true);

    best_of_n_result output;
    const auto count = run_options.policy.candidate_count;
    if (run_options.stop_token.stop_requested()) {
      output.stop_reason = best_of_n_stop_reason::cancelled;
      output.error = "best-of-n reasoning cancelled before candidate generation";
      output.candidates.reserve(count);
      for (std::size_t index = 0; index < count; ++index) {
        output.candidates.push_back({
          .index = index,
          .status = best_of_n_candidate_status::skipped,
          .request = request,
          .error = output.error,
        });
      }
      finalize(output, trace_state, started);
      return output;
    }

    struct candidate_seed {
      std::size_t index { 0 };
      reasoning_request request;
      std::string error;
    };
    auto remaining_timeout = remaining_time(started, run_options.policy.timeout);
    std::vector<std::size_t> indices(count);
    for (std::size_t index = 0; index < count; ++index) {
      indices[index] = index;
    }
    auto build_requests = fan_out_each(
      fan_out_options {
        .max_concurrency = 1,
        .failure_mode = fan_out_failure_mode::collect_all,
        .timeout = remaining_timeout,
      },
      [execution_options, request, side_effects = run_options.policy.side_effects](
        const std::size_t& index,
        const fan_out_context& fan_context) {
        candidate_seed seed { .index = index, .request = request };
        const best_of_n_context context {
          .index = index,
          .stop_token = fan_context.stop_token,
          .deadline = fan_context.deadline,
          .side_effects = side_effects,
        };
        try {
          if (execution_options->contextual_request_builder) {
            seed.request = execution_options->contextual_request_builder(
              request, index, context);
          }
          else if (execution_options->request_builder) {
            seed.request = execution_options->request_builder(request, index);
          }
          seed.request.metadata["best_of_n.candidate_index"] =
            std::to_string(index);
        }
        catch (const std::exception& ex) {
          seed.error = ex.what();
        }
        catch (...) {
          seed.error =
            "candidate request builder failed with an unknown exception";
        }
        return seed;
      });
    auto build_result = build_requests.run(
      std::move(indices), run_options.stop_token);
    if (build_result.stop_reason != fan_out_stop_reason::none) {
      trace_state->accept_worker_events = false;
      output.stop_reason = build_result.stop_reason == fan_out_stop_reason::timed_out
                             ? best_of_n_stop_reason::timed_out
                             : best_of_n_stop_reason::cancelled;
      output.error = output.stop_reason == best_of_n_stop_reason::timed_out
                       ? "best-of-n reasoning timed out while preparing candidates"
                       : "best-of-n reasoning cancelled while preparing candidates";
      output.candidates.reserve(count);
      for (auto& item : build_result.items) {
        best_of_n_candidate candidate {
          .index = item.index,
          .request = request,
          .error = item.error.empty() ? output.error : std::move(item.error),
          .elapsed = item.elapsed,
          .detached = item.detached,
        };
        if (item.status == fan_out_item_status::completed && item.value) {
          candidate.request = std::move(item.value->request);
          candidate.status = best_of_n_candidate_status::skipped;
        }
        else if (item.status == fan_out_item_status::timed_out) {
          candidate.status = best_of_n_candidate_status::timed_out;
        }
        else if (item.status == fan_out_item_status::cancelled) {
          candidate.status = best_of_n_candidate_status::cancelled;
        }
        else if (item.status == fan_out_item_status::failed) {
          candidate.status = best_of_n_candidate_status::generation_failed;
        }
        else {
          candidate.status = best_of_n_candidate_status::skipped;
        }
        output.candidates.push_back(std::move(candidate));
      }
      finalize(output, trace_state, started);
      return output;
    }

    std::vector<candidate_seed> seeds;
    seeds.reserve(count);
    for (auto& item : build_result.items) {
      if (item.status == fan_out_item_status::completed && item.value) {
        seeds.push_back(std::move(*item.value));
      }
      else {
        seeds.push_back({
          .index = item.index,
          .request = request,
          .error = item.error.empty()
                     ? "candidate request builder did not produce a result"
                     : std::move(item.error),
        });
      }
    }

    remaining_timeout = remaining_time(started, run_options.policy.timeout);
    if (run_options.policy.timeout.count() > 0 && remaining_timeout.count() == 0) {
      output.stop_reason = best_of_n_stop_reason::timed_out;
      output.error = "best-of-n reasoning timed out after preparing candidates";
      output.candidates.reserve(count);
      for (auto& seed : seeds) {
        output.candidates.push_back({
          .index = seed.index,
          .status = best_of_n_candidate_status::skipped,
          .request = std::move(seed.request),
          .error = seed.error.empty() ? output.error : std::move(seed.error),
        });
      }
      finalize(output, trace_state, started);
      return output;
    }

    const auto policy = run_options.policy;
    auto budget_state =
      std::make_shared<detail::best_of_n_budget_state>(policy.budget);
    auto parallel = fan_out_each(
      fan_out_options {
        .max_concurrency = policy.max_concurrency,
        .failure_mode = fan_out_failure_mode::collect_all,
        .timeout = remaining_timeout,
      },
      [execution_options, trace_state, budget_state, policy](
        const candidate_seed& seed,
        const fan_out_context& fan_context) {
        return evaluate_candidate(
          seed,
          fan_context,
          policy,
          execution_options,
          budget_state,
          trace_state);
      });
    auto parallel_result = parallel.run(std::move(seeds), run_options.stop_token);
    trace_state->accept_worker_events = false;

    output.candidates.reserve(count);
    for (auto& item : parallel_result.items) {
      if (item.status == fan_out_item_status::completed && item.value) {
        output.candidates.push_back(std::move(*item.value));
        continue;
      }
      best_of_n_candidate candidate {
        .index = item.index,
        .error = item.error,
        .elapsed = item.elapsed,
        .detached = item.detached,
      };
      switch (item.status) {
        case fan_out_item_status::failed:
          candidate.status = best_of_n_candidate_status::generation_failed;
          emit_candidate_terminal(
            trace_state, candidate, best_of_n_event_type::candidate_failed, true);
          break;
        case fan_out_item_status::cancelled:
          candidate.status = best_of_n_candidate_status::cancelled;
          emit_candidate_terminal(
            trace_state, candidate, best_of_n_event_type::candidate_cancelled, true);
          break;
        case fan_out_item_status::timed_out:
          candidate.status = best_of_n_candidate_status::timed_out;
          emit_candidate_terminal(
            trace_state, candidate, best_of_n_event_type::candidate_timed_out, true);
          break;
        case fan_out_item_status::skipped:
          candidate.status = best_of_n_candidate_status::skipped;
          emit_candidate_terminal(
            trace_state, candidate, best_of_n_event_type::candidate_skipped, true);
          break;
        case fan_out_item_status::completed:
          candidate.status = best_of_n_candidate_status::generation_failed;
          candidate.error = "candidate result was not available";
          emit_candidate_terminal(
            trace_state, candidate, best_of_n_event_type::candidate_failed, true);
          break;
      }
      output.candidates.push_back(std::move(candidate));
    }

    if (parallel_result.stop_reason == fan_out_stop_reason::cancelled ||
        run_options.stop_token.stop_requested()) {
      output.stop_reason = best_of_n_stop_reason::cancelled;
      output.error = "best-of-n reasoning cancelled";
    }
    else if (parallel_result.stop_reason == fan_out_stop_reason::timed_out ||
             deadline_reached(started, run_options.policy.timeout)) {
      output.stop_reason = best_of_n_stop_reason::timed_out;
      output.error = "best-of-n reasoning timed out";
    }
    else if (budget_state->exceeded()) {
      output.stop_reason = best_of_n_stop_reason::budget_exceeded;
      output.error = budget_state->error();
    }
    else {
      select_candidate(
        output,
        run_options.policy,
        execution_options,
        trace_state,
        started,
        run_options.stop_token);
    }

    const auto budget_snapshot = budget_state->snapshot();
    output.budget_accounted_usage = budget_snapshot.accounted_usage;
    output.outstanding_reserved_usage = budget_snapshot.outstanding_reserved_usage;

    finalize(output, trace_state, started);
    return output;
  }

  [[nodiscard]] best_of_n_run run_async(
    reasoning_request request,
    best_of_n_run_options run_options = {}) const {
    auto promise = std::make_shared<std::promise<best_of_n_result>>();
    auto future = promise->get_future();
    auto runner = *this;
    std::jthread worker(
      [runner = std::move(runner),
       request = std::move(request),
       run_options = std::move(run_options),
       promise](std::stop_token worker_stop_token) mutable {
        const auto external_stop_token = run_options.stop_token;
        std::stop_source run_stop_source;
        std::stop_callback external_stop_callback(
          external_stop_token,
          [&run_stop_source] { run_stop_source.request_stop(); });
        std::stop_callback worker_stop_callback(
          worker_stop_token,
          [&run_stop_source] { run_stop_source.request_stop(); });
        if (external_stop_token.stop_requested() ||
            worker_stop_token.stop_requested()) {
          run_stop_source.request_stop();
        }
        run_options.stop_token = run_stop_source.get_token();
        try {
          promise->set_value(
            runner.run(std::move(request), std::move(run_options)));
        }
        catch (...) {
          promise->set_exception(std::current_exception());
        }
      });
    return best_of_n_run(std::move(worker), std::move(future));
  }

private:
  template<typename Seed>
  static best_of_n_candidate evaluate_candidate(
    const Seed& seed,
    const fan_out_context& fan_context,
    const best_of_n_options& policy,
    const std::shared_ptr<const best_of_n_runner_options>& execution_options,
    const std::shared_ptr<detail::best_of_n_budget_state>& budget_state,
    const std::shared_ptr<detail::best_of_n_trace_state>& trace_state) {
    const auto started = std::chrono::steady_clock::now();
    best_of_n_candidate candidate {
      .index = seed.index,
      .request = seed.request,
    };
    const best_of_n_context context {
      .index = seed.index,
      .stop_token = fan_context.stop_token,
      .deadline = fan_context.deadline,
      .side_effects = policy.side_effects,
    };
    detail::emit_best_of_n_event(trace_state, {
      .type = best_of_n_event_type::candidate_started,
      .candidate_index = seed.index,
      .message = "candidate generation started",
    });

    if (!seed.error.empty()) {
      candidate.status = best_of_n_candidate_status::generation_failed;
      candidate.error = seed.error;
      finish_candidate(candidate, started);
      emit_candidate_terminal(trace_state, candidate, best_of_n_event_type::candidate_failed);
      return candidate;
    }
    if (context.cancellation_requested()) {
      candidate.status = best_of_n_candidate_status::cancelled;
      candidate.error = "candidate cancelled before generation";
      finish_candidate(candidate, started);
      emit_candidate_terminal(trace_state, candidate, best_of_n_event_type::candidate_cancelled);
      return candidate;
    }

    auto reservation = budget_state->try_reserve_candidate();
    if (!reservation) {
      candidate.status = best_of_n_candidate_status::budget_exceeded;
      candidate.error = budget_state->error();
      finish_candidate(candidate, started);
      emit_candidate_terminal(
        trace_state,
        candidate,
        best_of_n_event_type::candidate_budget_exceeded);
      return candidate;
    }

    try {
      candidate.result = execution_options->generator(candidate.request, context);
    }
    catch (const std::exception& ex) {
      reasoning_usage estimated_usage;
      estimated_usage.model_calls = reservation->model_calls;
      estimated_usage.total_tokens = reservation->total_tokens;
      estimated_usage.cost_usd = reservation->cost_usd;
      (void)budget_state->complete(*reservation, estimated_usage);
      candidate.status = best_of_n_candidate_status::generation_failed;
      candidate.error = ex.what();
      finish_candidate(candidate, started);
      emit_candidate_terminal(trace_state, candidate, best_of_n_event_type::candidate_failed);
      return candidate;
    }
    catch (...) {
      reasoning_usage estimated_usage;
      estimated_usage.model_calls = reservation->model_calls;
      estimated_usage.total_tokens = reservation->total_tokens;
      estimated_usage.cost_usd = reservation->cost_usd;
      (void)budget_state->complete(*reservation, estimated_usage);
      candidate.status = best_of_n_candidate_status::generation_failed;
      candidate.error = "candidate generator failed with an unknown exception";
      finish_candidate(candidate, started);
      emit_candidate_terminal(trace_state, candidate, best_of_n_event_type::candidate_failed);
      return candidate;
    }

    if (!budget_state->complete(*reservation, candidate.result.usage)) {
      candidate.status = best_of_n_candidate_status::budget_exceeded;
      candidate.error = budget_state->error();
      finish_candidate(candidate, started);
      emit_candidate_terminal(
        trace_state,
        candidate,
        best_of_n_event_type::candidate_budget_exceeded);
      return candidate;
    }

    if (policy.side_effects == best_of_n_side_effect_policy::isolate &&
        (candidate.result.usage.tool_calls != 0 ||
         candidate.result.usage.plan_steps != 0)) {
      candidate.status = best_of_n_candidate_status::side_effect_blocked;
      candidate.error =
        "candidate executed tools or plan steps while side effects were isolated";
      finish_candidate(candidate, started);
      emit_candidate_terminal(
        trace_state, candidate, best_of_n_event_type::candidate_failed);
      return candidate;
    }

    if (!candidate.result) {
      candidate.status = candidate.result.reasoning_error ==
                             reasoning_error_code::side_effect_blocked
                           ? best_of_n_candidate_status::side_effect_blocked
                           : best_of_n_candidate_status::generation_failed;
      candidate.error = candidate.result.error.empty()
                          ? "candidate generation did not complete"
                          : candidate.result.error;
      finish_candidate(candidate, started);
      emit_candidate_terminal(
        trace_state, candidate, best_of_n_event_type::candidate_failed);
      return candidate;
    }
    detail::emit_best_of_n_event(trace_state, {
      .type = best_of_n_event_type::candidate_generated,
      .candidate_index = seed.index,
      .message = "candidate generation completed",
      .metadata = candidate.result.final_response.metadata,
    });

    if (context.cancellation_requested()) {
      candidate.status = best_of_n_candidate_status::cancelled;
      candidate.error = "candidate cancelled before scoring";
      finish_candidate(candidate, started);
      emit_candidate_terminal(trace_state, candidate, best_of_n_event_type::candidate_cancelled);
      return candidate;
    }

    auto scorer_reservation = budget_state->try_reserve_scorer();
    if (!scorer_reservation) {
      candidate.status = best_of_n_candidate_status::budget_exceeded;
      candidate.error = budget_state->error();
      finish_candidate(candidate, started);
      emit_candidate_terminal(
        trace_state,
        candidate,
        best_of_n_event_type::candidate_budget_exceeded);
      return candidate;
    }
    try {
      candidate.score = execution_options->scorer(
        candidate.request, candidate.result, context);
      if (!std::isfinite(candidate.score->value)) {
        throw std::invalid_argument("candidate score must be finite");
      }
    }
    catch (const std::exception& ex) {
      reasoning_usage accounted_usage;
      if (candidate.score) {
        accounted_usage = candidate.score->usage;
        detail::merge_reasoning_usage(candidate.result.usage, accounted_usage);
      }
      else {
        accounted_usage.model_calls = scorer_reservation->model_calls;
        accounted_usage.total_tokens = scorer_reservation->total_tokens;
        accounted_usage.cost_usd = scorer_reservation->cost_usd;
      }
      (void)budget_state->complete(*scorer_reservation, accounted_usage);
      candidate.status = best_of_n_candidate_status::scoring_failed;
      candidate.score.reset();
      candidate.error = ex.what();
      finish_candidate(candidate, started);
      emit_candidate_terminal(trace_state, candidate, best_of_n_event_type::candidate_failed);
      return candidate;
    }
    catch (...) {
      reasoning_usage estimated_usage;
      estimated_usage.model_calls = scorer_reservation->model_calls;
      estimated_usage.total_tokens = scorer_reservation->total_tokens;
      estimated_usage.cost_usd = scorer_reservation->cost_usd;
      (void)budget_state->complete(*scorer_reservation, estimated_usage);
      candidate.status = best_of_n_candidate_status::scoring_failed;
      candidate.score.reset();
      candidate.error = "candidate scorer failed with an unknown exception";
      finish_candidate(candidate, started);
      emit_candidate_terminal(trace_state, candidate, best_of_n_event_type::candidate_failed);
      return candidate;
    }

    if (!budget_state->complete(*scorer_reservation, candidate.score->usage)) {
      detail::merge_reasoning_usage(
        candidate.result.usage, candidate.score->usage);
      candidate.status = best_of_n_candidate_status::budget_exceeded;
      candidate.error = budget_state->error();
      finish_candidate(candidate, started);
      emit_candidate_terminal(
        trace_state,
        candidate,
        best_of_n_event_type::candidate_budget_exceeded);
      return candidate;
    }
    detail::merge_reasoning_usage(candidate.result.usage, candidate.score->usage);

    const auto meets_minimum = !policy.minimum_score ||
      candidate.score->value >= *policy.minimum_score;
    if (!candidate.score->accepted || !meets_minimum) {
      candidate.status = best_of_n_candidate_status::rejected;
      candidate.error = !candidate.score->accepted
                          ? "candidate rejected by scorer"
                          : "candidate score is below the minimum";
      finish_candidate(candidate, started);
      emit_candidate_terminal(trace_state, candidate, best_of_n_event_type::candidate_rejected);
      return candidate;
    }

    candidate.status = best_of_n_candidate_status::eligible;
    finish_candidate(candidate, started);
    detail::emit_best_of_n_event(trace_state, {
      .type = best_of_n_event_type::candidate_scored,
      .candidate_index = seed.index,
      .score = candidate.score->value,
      .message = candidate.score->rationale,
      .metadata = candidate.score->metadata,
    });
    return candidate;
  }

  static void select_candidate(
    best_of_n_result& output,
    const best_of_n_options& policy,
    const std::shared_ptr<const best_of_n_runner_options>& execution_options,
    const std::shared_ptr<detail::best_of_n_trace_state>& trace_state,
    std::chrono::steady_clock::time_point started,
    std::stop_token stop_token) {
    const auto selection_timeout = remaining_time(started, policy.timeout);
    if (policy.timeout.count() > 0 && selection_timeout.count() == 0) {
      output.stop_reason = best_of_n_stop_reason::timed_out;
      output.error = "best-of-n reasoning timed out before candidate selection";
      return;
    }

    auto candidates =
      std::make_shared<const std::vector<best_of_n_candidate>>(output.candidates);
    auto selection = fan_out(
      fan_out_options {
        .max_concurrency = 1,
        .failure_mode = fan_out_failure_mode::collect_all,
        .timeout = selection_timeout,
      },
      [execution_options, policy](
        const std::shared_ptr<const std::vector<best_of_n_candidate>>& values,
        const fan_out_context& fan_context) {
        const best_of_n_context context {
          .index = 0,
          .stop_token = fan_context.stop_token,
          .deadline = fan_context.deadline,
          .side_effects = policy.side_effects,
        };
        if (execution_options->contextual_selector) {
          return execution_options->contextual_selector(*values, context);
        }
        if (execution_options->selector) {
          return execution_options->selector(*values);
        }
        return highest_scoring_candidate(*values, policy);
      });
    auto selection_result = selection.run(std::move(candidates), stop_token);
    const auto& item = selection_result.items.front();
    if (item.status != fan_out_item_status::completed || !item.value) {
      if (item.detached) {
        ++output.coordination_detached_count;
      }
      if (selection_result.stop_reason == fan_out_stop_reason::timed_out ||
          item.status == fan_out_item_status::timed_out) {
        output.stop_reason = best_of_n_stop_reason::timed_out;
        output.error = item.error.empty()
                         ? "best-of-n selector timed out"
                         : item.error;
      }
      else if (selection_result.stop_reason == fan_out_stop_reason::cancelled ||
               item.status == fan_out_item_status::cancelled) {
        output.stop_reason = best_of_n_stop_reason::cancelled;
        output.error = item.error.empty()
                         ? "best-of-n selector cancelled"
                         : item.error;
      }
      else {
        output.stop_reason = best_of_n_stop_reason::selection_failed;
        output.error = item.error.empty()
                         ? "best-of-n selector failed"
                         : item.error;
      }
      return;
    }
    output.selected_index = std::move(*item.value);

    if (!output.selected_index) {
      output.stop_reason = best_of_n_stop_reason::no_eligible_candidate;
      output.error = "best-of-n reasoning produced no eligible candidate";
      return;
    }
    if (*output.selected_index >= output.candidates.size() ||
        output.candidates[*output.selected_index].index != *output.selected_index ||
        !output.candidates[*output.selected_index].eligible()) {
      output.selected_index.reset();
      output.stop_reason = best_of_n_stop_reason::selection_failed;
      output.error = "best-of-n selector returned an ineligible candidate index";
      return;
    }

    output.completed = true;
    const auto& selected = output.candidates[*output.selected_index];
    detail::emit_best_of_n_event(trace_state, {
      .type = best_of_n_event_type::selected,
      .candidate_index = selected.index,
      .score = selected.score->value,
      .message = "best-of-n candidate selected",
    }, true);
  }

  static std::optional<std::size_t> highest_scoring_candidate(
    const std::vector<best_of_n_candidate>& candidates,
    const best_of_n_options& policy) {
    const best_of_n_candidate* best = nullptr;
    for (const auto& candidate : candidates) {
      if (!candidate.eligible()) {
        continue;
      }
      if (!best || candidate.score->value >
                     best->score->value + policy.score_tie_tolerance) {
        best = &candidate;
      }
      else if (std::abs(candidate.score->value - best->score->value) <=
                 policy.score_tie_tolerance &&
               policy.prefer_lower_cost_on_tie &&
               detail::lower_cost_candidate(candidate, *best)) {
        best = &candidate;
      }
    }
    return best ? std::optional(best->index) : std::nullopt;
  }

  static void validate(const best_of_n_options& value) {
    if (value.candidate_count == 0) {
      throw std::invalid_argument("best-of-n candidate_count must be greater than zero");
    }
    if (value.max_concurrency == 0) {
      throw std::invalid_argument("best-of-n max_concurrency must be greater than zero");
    }
    if (value.timeout.count() < 0) {
      throw std::invalid_argument("best-of-n timeout must not be negative");
    }
    if (value.minimum_score && !std::isfinite(*value.minimum_score)) {
      throw std::invalid_argument("best-of-n minimum_score must be finite");
    }
    if (!std::isfinite(value.score_tie_tolerance) ||
        value.score_tie_tolerance < 0.0) {
      throw std::invalid_argument(
        "best-of-n score_tie_tolerance must be finite and non-negative");
    }
    const auto& budget = value.budget;
    if (!std::isfinite(budget.max_cost_usd) || budget.max_cost_usd < 0.0 ||
        !std::isfinite(budget.estimated_cost_usd_per_candidate) ||
        budget.estimated_cost_usd_per_candidate < 0.0 ||
        !std::isfinite(budget.estimated_scorer_cost_usd_per_candidate) ||
        budget.estimated_scorer_cost_usd_per_candidate < 0.0) {
      throw std::invalid_argument(
        "best-of-n cost budgets and estimates must be finite and non-negative");
    }
    const auto combined_estimate = [](std::size_t generation, std::size_t scoring) {
      const auto maximum = (std::numeric_limits<std::size_t>::max)();
      if (scoring > maximum - generation) {
        throw std::invalid_argument("best-of-n per-candidate budget estimate overflowed");
      }
      return generation + scoring;
    };
    const auto model_calls_per_candidate = combined_estimate(
      budget.estimated_model_calls_per_candidate,
      budget.estimated_scorer_model_calls_per_candidate);
    const auto total_tokens_per_candidate = combined_estimate(
      budget.estimated_total_tokens_per_candidate,
      budget.estimated_scorer_total_tokens_per_candidate);
    const auto cost_per_candidate =
      budget.estimated_cost_usd_per_candidate +
      budget.estimated_scorer_cost_usd_per_candidate;
    if (!std::isfinite(cost_per_candidate)) {
      throw std::invalid_argument("best-of-n per-candidate cost estimate overflowed");
    }
    if (budget.max_model_calls != 0 &&
        (model_calls_per_candidate == 0 ||
         model_calls_per_candidate >
           budget.max_model_calls / value.candidate_count)) {
      throw std::invalid_argument(
        "best-of-n model call budget cannot cover every requested candidate");
    }
    if (budget.max_total_tokens != 0 &&
        (total_tokens_per_candidate == 0 ||
         total_tokens_per_candidate >
           budget.max_total_tokens / value.candidate_count)) {
      throw std::invalid_argument(
        "best-of-n token budget requires a non-zero estimate and capacity for every candidate");
    }
    if (budget.max_cost_usd > 0.0 &&
        (cost_per_candidate <= 0.0 ||
         cost_per_candidate *
             static_cast<double>(value.candidate_count) >
           budget.max_cost_usd + 1e-12)) {
      throw std::invalid_argument(
        "best-of-n cost budget requires a non-zero estimate and capacity for every candidate");
    }
  }

  static bool deadline_reached(
    std::chrono::steady_clock::time_point started,
    std::chrono::milliseconds timeout) noexcept {
    return timeout.count() > 0 &&
           std::chrono::steady_clock::now() - started >= timeout;
  }

  static std::chrono::milliseconds remaining_time(
    std::chrono::steady_clock::time_point started,
    std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() == 0) {
      return std::chrono::milliseconds { 0 };
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
    return elapsed >= timeout ? std::chrono::milliseconds { 0 } : timeout - elapsed;
  }

  static void finish_candidate(
    best_of_n_candidate& candidate,
    std::chrono::steady_clock::time_point started) noexcept {
    candidate.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  }

  static void emit_candidate_terminal(
    const std::shared_ptr<detail::best_of_n_trace_state>& trace_state,
    const best_of_n_candidate& candidate,
    best_of_n_event_type type,
    bool coordinator_event = false) {
    detail::emit_best_of_n_event(trace_state, {
      .type = type,
      .candidate_index = candidate.index,
      .score = candidate.score
                 ? std::optional(candidate.score->value)
                 : std::nullopt,
      .message = candidate.error,
    }, coordinator_event);
  }

  static void finalize(
    best_of_n_result& output,
    const std::shared_ptr<detail::best_of_n_trace_state>& trace_state,
    std::chrono::steady_clock::time_point started) {
    for (const auto& candidate : output.candidates) {
      detail::merge_reasoning_usage(output.aggregate_usage, candidate.result.usage);
      switch (candidate.status) {
        case best_of_n_candidate_status::eligible: ++output.eligible_count; break;
        case best_of_n_candidate_status::rejected: ++output.rejected_count; break;
        case best_of_n_candidate_status::generation_failed:
        case best_of_n_candidate_status::scoring_failed:
          ++output.failed_count;
          break;
        case best_of_n_candidate_status::budget_exceeded:
          ++output.budget_exceeded_count;
          break;
        case best_of_n_candidate_status::side_effect_blocked:
          ++output.side_effect_blocked_count;
          break;
        case best_of_n_candidate_status::cancelled: ++output.cancelled_count; break;
        case best_of_n_candidate_status::timed_out: ++output.timed_out_count; break;
        case best_of_n_candidate_status::skipped: ++output.skipped_count; break;
      }
      if (candidate.detached) {
        ++output.detached_count;
      }
    }
    output.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
    const auto terminal_type = output.completed
                                 ? best_of_n_event_type::completed
                                 : (output.stop_reason == best_of_n_stop_reason::cancelled
                                      ? best_of_n_event_type::cancelled
                                      : best_of_n_event_type::failed);
    // Stop worker-originated callbacks before emitting the terminal event.
    // The observer mutex taken by the coordinator event then also waits for
    // any callback that was already in flight, so no observer can outlive run().
    trace_state->accept_worker_events = false;
    detail::emit_best_of_n_event(trace_state, {
      .type = terminal_type,
      .candidate_index = output.selected_index,
      .message = output.completed
                   ? "best-of-n reasoning completed"
                   : output.error,
    }, true);
    output.telemetry_error_count =
      trace_state->telemetry_error_count.load(std::memory_order_relaxed);
    {
      std::scoped_lock lock(trace_state->mutex);
      output.trace = trace_state->trace;
    }
  }

  best_of_n_runner_options options_;
};

} // namespace wuwe::agent::reasoning

#include <wuwe/agent/reasoning/best_of_n_utilities.hpp>

#endif // WUWE_AGENT_REASONING_BEST_OF_N_HPP
