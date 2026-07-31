#ifndef WUWE_AGENT_PLANNING_PLAN_RUNNER_HPP
#define WUWE_AGENT_PLANNING_PLAN_RUNNER_HPP

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
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

#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/agent/planning/plan.hpp>
#include <wuwe/agent/planning/plan_executor.hpp>
#include <wuwe/agent/planning/plan_prioritizer.hpp>
#include <wuwe/agent/planning/plan_reflection.hpp>
#include <wuwe/agent/planning/plan_store.hpp>
#include <wuwe/agent/planning/planner.hpp>

namespace wuwe::agent::planning {

enum class plan_event_type {
  plan_created,
  plan_resumed,
  step_started,
  step_completed,
  step_failed,
  step_blocked,
  step_reflected,
  step_approval_required,
  plan_revised,
  plan_cancelled,
  plan_finished,
  plan_timed_out,
};

struct plan_event {
  plan_event_type type;
  const plan* current_plan {};
  const plan_step* step {};
  planning_observation observation;
};

using plan_observer = std::function<void(const plan_event&)>;

struct plan_trace_event {
  plan_event_type type;
  std::string plan_id;
  std::string step_id;
  std::size_t iteration {};
  std::chrono::milliseconds elapsed { 0 };
  std::map<std::string, std::string> metadata;
};

using plan_trace_sink = std::function<void(const plan_trace_event&)>;

class plan_runtime_services {
public:
  plan_runtime_services(const plan_store* store, agent::memory::memory_context* memory,
    plan_observer observer, plan_trace_sink trace_sink)
      : store_(const_cast<plan_store*>(store)), memory_(memory), observer_(std::move(observer)),
        trace_sink_(std::move(trace_sink)) {
  }

  void notify(plan_event event) const {
    if (observer_) {
      observer_(event);
    }
  }

  void trace(plan_event_type type, const plan& value, const plan_step* step, std::size_t iteration,
    std::chrono::milliseconds elapsed) const {
    if (!trace_sink_) {
      return;
    }
    trace_sink_({
      .type = type,
      .plan_id = value.id,
      .step_id = step == nullptr ? std::string {} : step->id,
      .iteration = iteration,
      .elapsed = elapsed,
    });
  }

  void remember(std::string content, std::map<std::string, std::string> metadata) const {
    if (memory_) {
      memory_->remember_working(std::move(content), std::move(metadata));
    }
  }

  void remember_step(const std::string& plan_id, const plan_step& step) const {
    remember("plan step '" + step.id + "' " + to_string(step.status) +
               (step.error.empty() ? std::string {} : ": " + step.error),
      {
        { "planning_event", "step_finished" },
        { "plan_id", plan_id },
        { "step_id", step.id },
        { "status", to_string(step.status) },
      });
  }

  void save(const plan& value) const {
    if (store_) {
      store_->save(value);
    }
  }

private:
  plan_store* store_ {};
  agent::memory::memory_context* memory_ {};
  plan_observer observer_;
  plan_trace_sink trace_sink_;
};

class plan_graph {
public:
  static bool dependency_completed(const plan& value, const std::string& id) {
    const auto found = std::find_if(
      value.steps.begin(), value.steps.end(), [&](const plan_step& step) { return step.id == id; });
    return found != value.steps.end() && found->status == plan_step_status::completed;
  }

  static bool dependencies_completed(const plan& value, const plan_step& step) {
    return std::all_of(step.depends_on.begin(), step.depends_on.end(), [&](const std::string& id) {
      return dependency_completed(value, id);
    });
  }

  static bool step_approval_satisfied(const plan_step& step) {
    return !step.requires_approval || step.approved;
  }

  static std::vector<std::size_t> ready_step_indices(const plan& value) {
    std::vector<std::size_t> output;
    for (std::size_t index = 0; index < value.steps.size(); ++index) {
      const auto& step = value.steps[index];
      if (step.status == plan_step_status::pending && dependencies_completed(value, step) &&
          step_approval_satisfied(step)) {
        output.push_back(index);
      }
    }
    return output;
  }

  static bool all_terminal(const plan& value) {
    return std::all_of(value.steps.begin(), value.steps.end(), [](const plan_step& step) {
      return is_terminal(step.status);
    });
  }

  static bool has_status(const plan& value, plan_step_status status) {
    return std::any_of(value.steps.begin(), value.steps.end(), [&](const plan_step& step) {
      return step.status == status;
    });
  }

  static const plan_step* first_pending_approval_step(const plan& value) {
    const auto found =
      std::find_if(value.steps.begin(), value.steps.end(), [&](const plan_step& step) {
        return step.status == plan_step_status::pending && dependencies_completed(value, step) &&
               step.requires_approval && !step.approved;
      });
    return found == value.steps.end() ? nullptr : &*found;
  }

  static plan_step* find_step(plan& value, const std::string& id) {
    const auto found = std::find_if(
      value.steps.begin(), value.steps.end(), [&](const plan_step& step) { return step.id == id; });
    return found == value.steps.end() ? nullptr : &*found;
  }
};

class plan_step_state {
public:
  static void apply_result(plan& value, plan_step& step, plan_step_result result) {
    step.status = result.status;
    step.output = std::move(result.output);
    step.output_json = std::move(result.output_json);
    step.error = std::move(result.error);
    for (auto& [key, artifact] : result.artifacts) {
      value.artifacts[key] = std::move(artifact);
      step.produced_artifacts.push_back(key);
      step.metadata["artifact:" + key] = "produced";
    }
    for (auto& [key, metadata_value] : result.metadata) {
      step.metadata[key] = std::move(metadata_value);
    }
    if (step.status == plan_step_status::running || step.status == plan_step_status::pending) {
      step.status = plan_step_status::completed;
    }
  }

  static void hydrate_input(plan& value, plan_step& step) {
    if (step.input_from_steps.empty()) {
      return;
    }

    nlohmann::json input = step.input_json.is_null() ? nlohmann::json::object() : step.input_json;
    if (!input.is_object()) {
      input = nlohmann::json::object();
    }
    for (const auto& source_id : step.input_from_steps) {
      const auto found = std::find_if(value.steps.begin(),
        value.steps.end(),
        [&](const plan_step& candidate) { return candidate.id == source_id; });
      if (found != value.steps.end()) {
        input["steps"][source_id] =
          found->output_json.is_null() ? nlohmann::json(found->output) : found->output_json;
      }
    }
    step.input_json = std::move(input);
    if (step.input.empty()) {
      step.input = step.input_json.dump();
    }
  }

  static std::string final_output(const plan& value) {
    for (auto it = value.steps.rbegin(); it != value.steps.rend(); ++it) {
      if (it->status == plan_step_status::completed && !it->output.empty()) {
        return it->output;
      }
    }
    return {};
  }
};

class plan_step_recovery_policy {
public:
  explicit plan_step_recovery_policy(const plan_policy& policy) : policy_(policy) {
  }

  bool should_retry(const plan_step& step) const {
    if (!failed_or_blocked(step) || detached_execution(step) ||
        step.attempts >= policy_.max_step_attempts) {
      return false;
    }
    const auto action = plan_reflection_metadata::action_for(step);
    return !action || *action == agent::reflection::reflection_action::retry;
  }

  bool should_replan(const plan_step& step) const {
    if (!policy_.allow_replanning || !failed_or_blocked(step) || detached_execution(step)) {
      return false;
    }
    const auto action = plan_reflection_metadata::action_for(step);
    return !action || *action == agent::reflection::reflection_action::replan;
  }

private:
  static bool detached_execution(const plan_step& step) {
    const auto found = step.metadata.find("execution_detached");
    return found != step.metadata.end() && found->second == "true";
  }

  static bool failed_or_blocked(const plan_step& step) {
    return step.status == plan_step_status::failed || step.status == plan_step_status::blocked;
  }

  const plan_policy& policy_;
};

struct plan_runner_options {
  std::shared_ptr<::wuwe::agent::planning::planner> planner;
  std::shared_ptr<plan_executor> executor;
  plan_store* store {};
  agent::memory::memory_context* memory {};
  plan_policy policy;
  plan_priority_scorer priority_scorer;
  bool validate_plans { true };
  plan_validation_options validation;
  std::function<plan_policy_check(const plan_step&, const plan&)> policy_check;
  std::function<plan_approval_result(const plan_step&, const plan&)> approval_provider;
  plan_reflection_options reflection;
  std::stop_token stop_token;
  std::function<bool()> should_cancel;
  plan_observer observer;
  plan_trace_sink trace_sink;
};

class plan_runner {
public:
  explicit plan_runner(plan_runner_options options) : options_(std::move(options)) {
    if (!options_.planner) {
      throw std::invalid_argument("plan_runner requires planner");
    }
    if (!options_.executor) {
      throw std::invalid_argument("plan_runner requires executor");
    }
    if (options_.policy.max_iterations == 0) {
      throw std::invalid_argument("plan max_iterations must be greater than zero");
    }
    if (options_.policy.max_step_attempts == 0) {
      throw std::invalid_argument("plan max_step_attempts must be greater than zero");
    }
    if (options_.policy.step_timeout.count() < 0 || options_.policy.run_timeout.count() < 0) {
      throw std::invalid_argument("plan timeouts must not be negative");
    }
    if (options_.policy.cancellation_poll_interval.count() <= 0) {
      throw std::invalid_argument("plan cancellation poll interval must be greater than zero");
    }
    (void)plan_prioritizer(options_.policy.prioritization, options_.priority_scorer);
  }

  plan_run_result run(planning_request request) {
    if (request.max_steps == 0) {
      request.max_steps = options_.policy.max_steps;
    }

    plan current = options_.planner->create_plan(request);
    validate_or_throw(current, validation_for(request));
    notify({ .type = plan_event_type::plan_created, .current_plan = &current });
    trace(plan_event_type::plan_created, current, nullptr, 0, {});
    remember("created plan '" + current.id + "' for goal: " + current.goal,
      { { "planning_event", "plan_created" }, { "plan_id", current.id } });
    save(current);

    return run_plan(std::move(current), request);
  }

  plan_run_result resume(plan current, planning_request request = {}) {
    if (request.goal.empty()) {
      request.goal = current.goal;
    }
    if (request.max_steps == 0) {
      request.max_steps = options_.policy.max_steps;
    }

    if (options_.policy.reset_running_steps_on_resume) {
      for (auto& step : current.steps) {
        if (step.status == plan_step_status::running) {
          const auto detached = step.metadata.find("execution_detached");
          if (!options_.policy.reset_detached_steps_on_resume && detached != step.metadata.end() &&
              detached->second == "true") {
            continue;
          }
          step.status = plan_step_status::pending;
          clear_execution_interruption_metadata(step);
        }
      }
    }

    validate_or_throw(current, validation_for(request));
    notify({ .type = plan_event_type::plan_resumed, .current_plan = &current });
    trace(plan_event_type::plan_resumed, current, nullptr, 0, {});
    remember("resumed plan '" + current.id + "' for goal: " + current.goal,
      { { "planning_event", "plan_resumed" }, { "plan_id", current.id } });
    save(current);

    return run_plan(std::move(current), request);
  }

private:
  enum class run_interruption {
    none,
    cancelled,
    run_timeout,
  };

  struct execution_batch_result {
    std::vector<planning_observation> observations;
    run_interruption interruption { run_interruption::none };
  };

  class execution_batch_signal {
  public:
    void notify() {
      {
        std::scoped_lock lock(mutex_);
        ++generation_;
      }
      condition_.notify_all();
    }

    void wait_until(std::stop_token stop_token, std::chrono::steady_clock::time_point deadline) {
      std::unique_lock lock(mutex_);
      const auto observed_generation = generation_;
      condition_.wait_until(
        lock, stop_token, deadline, [&] { return generation_ != observed_generation; });
    }

  private:
    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::size_t generation_ {};
  };

  struct plan_step_execution_item {
    struct task_result {
      plan_step_result result;
      std::chrono::steady_clock::time_point completed_at;
    };

    std::size_t index {};
    std::future<task_result> future;
    std::stop_source stop_source;
    std::chrono::steady_clock::time_point started;
    std::optional<std::chrono::steady_clock::time_point> step_deadline;
    plan_executor_capabilities capabilities;
    bool settled { false };
  };

  plan_run_result run_plan(plan current, const planning_request& request) {
    const auto run_started = std::chrono::steady_clock::now();
    const auto run_deadline = options_.policy.run_timeout.count() > 0
                                ? std::optional(run_started + options_.policy.run_timeout)
                                : std::nullopt;
    std::stop_source run_stop_source;
    std::stop_callback external_stop_callback(
      options_.stop_token, [&run_stop_source] { run_stop_source.request_stop(); });
    if (options_.stop_token.stop_requested()) {
      run_stop_source.request_stop();
    }

    plan_run_result result;
    result.value = std::move(current);

    for (std::size_t iteration = 0; iteration < options_.policy.max_iterations; ++iteration) {
      result.iterations = iteration + 1;
      result.elapsed = elapsed_since(run_started);

      if (const auto interruption = current_run_interruption(run_stop_source, run_deadline);
          interruption != run_interruption::none) {
        return finish_interrupted_run(std::move(result), interruption, run_started);
      }

      if (options_.policy.max_steps_per_run != 0 &&
          result.steps_executed >= options_.policy.max_steps_per_run) {
        result.completed = false;
        result.stop_reason = plan_run_stop_reason::step_budget_exhausted;
        result.final_output = final_output(result.value);
        notify({ .type = plan_event_type::plan_finished, .current_plan = &result.value });
        trace(
          plan_event_type::plan_finished, result.value, nullptr, result.iterations, result.elapsed);
        save(result.value);
        return result;
      }

      resolve_approvals(result.value);
      if (const auto interruption = current_run_interruption(run_stop_source, run_deadline);
          interruption != run_interruption::none) {
        return finish_interrupted_run(std::move(result), interruption, run_started);
      }

      auto ready = plan_prioritizer(options_.policy.prioritization, options_.priority_scorer)
                     .order(result.value, plan_graph::ready_step_indices(result.value));
      if (ready.empty()) {
        result.completed = plan_graph::all_terminal(result.value) &&
                           !plan_graph::has_status(result.value, plan_step_status::failed) &&
                           !plan_graph::has_status(result.value, plan_step_status::blocked);
        result.stop_reason = stop_reason_without_ready_step(result.value, result.completed);
        result.final_output = final_output(result.value);
        if (!result.completed && result.error.empty()) {
          result.error = result.stop_reason == plan_run_stop_reason::approval_required
                           ? "plan has a pending step awaiting approval"
                           : "plan has no executable pending step";
        }
        if (result.stop_reason == plan_run_stop_reason::approval_required) {
          if (const auto* approval_step = plan_graph::first_pending_approval_step(result.value)) {
            notify({ .type = plan_event_type::step_approval_required,
              .current_plan = &result.value,
              .step = approval_step });
            trace(plan_event_type::step_approval_required,
              result.value,
              approval_step,
              result.iterations,
              result.elapsed);
          }
        }
        notify({ .type = plan_event_type::plan_finished, .current_plan = &result.value });
        trace(
          plan_event_type::plan_finished, result.value, nullptr, result.iterations, result.elapsed);
        save(result.value);
        return result;
      }

      const auto budget = remaining_step_budget(result);
      if (budget != 0 && ready.size() > budget) {
        ready.resize(budget);
      }
      const auto parallelism =
        (std::max<std::size_t>)(std::size_t { 1 }, options_.policy.max_parallel_steps);
      if (ready.size() > parallelism) {
        ready.resize(parallelism);
      }
      if (ready.size() > 1 && std::any_of(ready.begin(), ready.end(), [&](std::size_t index) {
            return !options_.executor->capabilities(result.value.steps[index]).concurrent_execution;
          })) {
        ready.resize(1);
      }

      auto batch = execute_ready_steps(
        result.value, ready, result, run_started, run_stop_source, run_deadline);
      save(result.value);

      if (batch.interruption != run_interruption::none) {
        return finish_interrupted_run(std::move(result), batch.interruption, run_started);
      }

      for (const auto& observation : batch.observations) {
        auto* step = plan_graph::find_step(result.value, observation.step_id);
        if (step == nullptr) {
          continue;
        }

        if (recovery_policy().should_retry(*step)) {
          step->status = plan_step_status::pending;
          continue;
        }

        if (recovery_policy().should_replan(*step)) {
          result.value = options_.planner->revise_plan(result.value, observation);
          validate_or_throw(result.value, validation_for(request));
          notify({ .type = plan_event_type::plan_revised, .current_plan = &result.value });
          trace(plan_event_type::plan_revised,
            result.value,
            nullptr,
            result.iterations,
            result.elapsed);
          save(result.value);
          break;
        }

        if ((step->status == plan_step_status::failed ||
              step->status == plan_step_status::blocked) &&
            !options_.policy.continue_after_step_failure) {
          result.completed = false;
          result.stop_reason = step->status == plan_step_status::blocked
                                 ? plan_run_stop_reason::blocked
                                 : plan_run_stop_reason::failed;
          result.error = step->error.empty() ? "plan step failed: " + step->id : step->error;
          result.final_output = final_output(result.value);
          result.elapsed = elapsed_since(run_started);
          notify({ .type = plan_event_type::plan_finished, .current_plan = &result.value });
          trace(plan_event_type::plan_finished,
            result.value,
            nullptr,
            result.iterations,
            result.elapsed);
          save(result.value);
          return result;
        }
      }
    }

    result.completed = false;
    result.stop_reason = plan_run_stop_reason::max_iterations;
    result.error = "plan exceeded maximum iterations";
    result.final_output = final_output(result.value);
    result.elapsed = elapsed_since(run_started);
    notify({ .type = plan_event_type::plan_finished, .current_plan = &result.value });
    trace(plan_event_type::plan_finished, result.value, nullptr, result.iterations, result.elapsed);
    save(result.value);
    return result;
  }

  run_interruption current_run_interruption(std::stop_source& run_stop_source,
    const std::optional<std::chrono::steady_clock::time_point>& run_deadline) const {
    if (options_.stop_token.stop_requested()) {
      run_stop_source.request_stop();
      return run_interruption::cancelled;
    }
    if (options_.should_cancel && options_.should_cancel()) {
      run_stop_source.request_stop();
      return run_interruption::cancelled;
    }
    if (run_deadline && std::chrono::steady_clock::now() >= *run_deadline) {
      run_stop_source.request_stop();
      return run_interruption::run_timeout;
    }
    if (run_stop_source.stop_requested()) {
      return run_interruption::cancelled;
    }
    return run_interruption::none;
  }

  plan_run_result finish_interrupted_run(plan_run_result result, run_interruption interruption,
    std::chrono::steady_clock::time_point run_started) const {
    result.completed = false;
    result.stop_reason = interruption == run_interruption::run_timeout
                           ? plan_run_stop_reason::run_timeout
                           : plan_run_stop_reason::cancelled;
    result.error = interruption == run_interruption::run_timeout ? "plan run timeout exceeded"
                                                                 : "plan run cancelled";
    result.final_output = final_output(result.value);
    result.elapsed = elapsed_since(run_started);
    const auto event_type = interruption == run_interruption::run_timeout
                              ? plan_event_type::plan_timed_out
                              : plan_event_type::plan_cancelled;
    notify({ .type = event_type, .current_plan = &result.value });
    trace(event_type, result.value, nullptr, result.iterations, result.elapsed);
    notify({ .type = plan_event_type::plan_finished, .current_plan = &result.value });
    trace(plan_event_type::plan_finished, result.value, nullptr, result.iterations, result.elapsed);
    save(result.value);
    return result;
  }

  void resolve_approvals(plan& value) const {
    if (!options_.approval_provider) {
      return;
    }
    for (auto& step : value.steps) {
      if (step.status != plan_step_status::pending ||
          !plan_graph::dependencies_completed(value, step) || !step.requires_approval ||
          step.approved) {
        continue;
      }
      const auto approval = options_.approval_provider(step, value);
      if (approval.approved) {
        step.approved = true;
        for (const auto& [key, metadata_value] : approval.metadata) {
          step.metadata[key] = metadata_value;
        }
        if (!approval.reason.empty()) {
          step.metadata["approval_reason"] = approval.reason;
        }
      }
    }
  }

  std::size_t remaining_step_budget(const plan_run_result& result) const {
    if (options_.policy.max_steps_per_run == 0) {
      return 0;
    }
    if (result.steps_executed >= options_.policy.max_steps_per_run) {
      return 0;
    }
    return options_.policy.max_steps_per_run - result.steps_executed;
  }

  execution_batch_result execute_ready_steps(plan& value, const std::vector<std::size_t>& ready,
    plan_run_result& run_result, std::chrono::steady_clock::time_point run_started,
    std::stop_source& run_stop_source,
    const std::optional<std::chrono::steady_clock::time_point>& run_deadline) {
    std::vector<plan_step_execution_item> running;
    std::vector<std::size_t> launch_indices;
    execution_batch_result batch;

    for (const auto index : ready) {
      if (const auto interruption = current_run_interruption(run_stop_source, run_deadline);
          interruption != run_interruption::none) {
        batch.interruption = interruption;
        return batch;
      }

      auto& step = value.steps[index];

      if (options_.policy_check) {
        const auto decision = options_.policy_check(step, value);
        if (decision.decision == plan_policy_decision::deny) {
          ++step.attempts;
          apply_step_result(value,
            step,
            plan_step_result::blocked(
              decision.reason.empty() ? "step denied by planning policy" : decision.reason));
          batch.observations.push_back(observe_finished_step(value, step, run_result, run_started));
          continue;
        }
        if (decision.decision == plan_policy_decision::require_approval) {
          step.requires_approval = true;
          step.approved = false;
          if (!decision.reason.empty()) {
            step.metadata["approval_reason"] = decision.reason;
          }
          notify({ .type = plan_event_type::step_approval_required,
            .current_plan = &value,
            .step = &step });
          trace(plan_event_type::step_approval_required,
            value,
            &step,
            run_result.iterations,
            elapsed_since(run_started));
          continue;
        }
      }

      if (const auto interruption = current_run_interruption(run_stop_source, run_deadline);
          interruption != run_interruption::none) {
        batch.interruption = interruption;
        return batch;
      }

      hydrate_step_input(value, step);
      clear_execution_interruption_metadata(step);
      launch_indices.push_back(index);
    }

    if (launch_indices.empty()) {
      return batch;
    }

    for (const auto index : launch_indices) {
      auto& step = value.steps[index];
      step.status = plan_step_status::running;
      ++step.attempts;
      ++run_result.steps_executed;
      notify({ .type = plan_event_type::step_started, .current_plan = &value, .step = &step });
      trace(plan_event_type::step_started,
        value,
        &step,
        run_result.iterations,
        elapsed_since(run_started));
    }

    const auto snapshot = std::make_shared<const plan>(value);
    const auto executor = options_.executor;
    const auto signal = std::make_shared<execution_batch_signal>();
    running.reserve(launch_indices.size());

    for (const auto index : launch_indices) {
      const auto started = std::chrono::steady_clock::now();
      const auto step_deadline = options_.policy.step_timeout.count() > 0
                                   ? std::optional(started + options_.policy.step_timeout)
                                   : std::nullopt;
      const auto deadline = earliest_deadline(step_deadline, run_deadline);
      std::stop_source step_stop_source;
      const auto stop_token = step_stop_source.get_token();
      auto promise = std::make_shared<std::promise<plan_step_execution_item::task_result>>();
      auto future = promise->get_future();
      const auto step = snapshot->steps[index];
      const auto capabilities = executor->capabilities(step);

      try {
        std::thread worker(
          [executor, snapshot, signal, step, stop_token, deadline, promise]() mutable {
            plan_step_result result;
            try {
              result = executor->execute(step,
                {
                  .current_plan = *snapshot,
                  .artifacts = snapshot->artifacts,
                  .stop_token = stop_token,
                  .deadline = deadline,
                });
            }
            catch (const std::exception& ex) {
              result = plan_step_result::failed(
                std::string("plan step executor threw an exception: ") + ex.what());
              result.metadata["executor_exception"] = "true";
            }
            catch (...) {
              result = plan_step_result::failed("plan step executor threw an unknown exception");
              result.metadata["executor_exception"] = "true";
            }
            try {
              promise->set_value({
                .result = std::move(result),
                .completed_at = std::chrono::steady_clock::now(),
              });
            }
            catch (...) {
            }
            signal->notify();
          });
        try {
          worker.detach();
        }
        catch (...) {
          if (worker.joinable())
            worker.join();
        }
      }
      catch (const std::exception& ex) {
        auto result =
          plan_step_result::failed(std::string("failed to start plan step executor: ") + ex.what());
        result.metadata["executor_start_exception"] = "true";
        promise->set_value({
          .result = std::move(result),
          .completed_at = std::chrono::steady_clock::now(),
        });
        signal->notify();
      }
      catch (...) {
        auto result =
          plan_step_result::failed("failed to start plan step executor with an unknown exception");
        result.metadata["executor_start_exception"] = "true";
        promise->set_value({
          .result = std::move(result),
          .completed_at = std::chrono::steady_clock::now(),
        });
        signal->notify();
      }

      running.push_back({
        .index = index,
        .future = std::move(future),
        .stop_source = std::move(step_stop_source),
        .started = started,
        .step_deadline = step_deadline,
        .capabilities = capabilities,
      });
    }

    std::size_t remaining = running.size();
    while (remaining > 0) {
      for (auto& item : running) {
        if (item.settled ||
            item.future.wait_for(std::chrono::milliseconds { 0 }) != std::future_status::ready) {
          continue;
        }
        const auto completion_interruption = settle_completed_step(
          value, item, run_result, run_started, batch.observations, run_deadline);
        --remaining;
        if (completion_interruption != run_interruption::none) {
          run_stop_source.request_stop();
          interrupt_running_steps(value,
            running,
            run_result,
            run_started,
            batch.observations,
            completion_interruption,
            run_deadline);
          batch.interruption = completion_interruption;
          return batch;
        }
      }

      if (remaining == 0) {
        break;
      }

      if (const auto interruption = current_run_interruption(run_stop_source, run_deadline);
          interruption != run_interruption::none) {
        interrupt_running_steps(
          value, running, run_result, run_started, batch.observations, interruption, run_deadline);
        batch.interruption = interruption;
        return batch;
      }

      const auto now = std::chrono::steady_clock::now();
      for (auto& item : running) {
        if (item.settled || !item.step_deadline || now < *item.step_deadline) {
          continue;
        }
        if (item.future.wait_for(std::chrono::milliseconds { 0 }) == std::future_status::ready) {
          const auto completion_interruption = settle_completed_step(
            value, item, run_result, run_started, batch.observations, run_deadline);
          --remaining;
          if (completion_interruption != run_interruption::none) {
            run_stop_source.request_stop();
            interrupt_running_steps(value,
              running,
              run_result,
              run_started,
              batch.observations,
              completion_interruption,
              run_deadline);
            batch.interruption = completion_interruption;
            return batch;
          }
          continue;
        }
        settle_timed_out_step(value, item, run_result, run_started, batch.observations);
        --remaining;
      }

      if (remaining == 0) {
        break;
      }

      signal->wait_until(run_stop_source.get_token(), next_wake_deadline(running, run_deadline));
    }

    return batch;
  }

  static std::optional<std::chrono::steady_clock::time_point> earliest_deadline(
    const std::optional<std::chrono::steady_clock::time_point>& first,
    const std::optional<std::chrono::steady_clock::time_point>& second) {
    if (!first) {
      return second;
    }
    if (!second) {
      return first;
    }
    return (std::min)(*first, *second);
  }

  std::chrono::steady_clock::time_point next_wake_deadline(
    const std::vector<plan_step_execution_item>& running,
    const std::optional<std::chrono::steady_clock::time_point>& run_deadline) const {
    const auto poll_interval = options_.policy.cancellation_poll_interval.count() > 0
                                 ? options_.policy.cancellation_poll_interval
                                 : std::chrono::milliseconds { 1 };
    auto wake = std::chrono::steady_clock::now() + poll_interval;
    if (run_deadline) {
      wake = (std::min)(wake, *run_deadline);
    }
    for (const auto& item : running) {
      if (!item.settled && item.step_deadline) {
        wake = (std::min)(wake, *item.step_deadline);
      }
    }
    return wake;
  }

  run_interruption settle_completed_step(plan& value, plan_step_execution_item& item,
    plan_run_result& run_result, std::chrono::steady_clock::time_point run_started,
    std::vector<planning_observation>& observations,
    const std::optional<std::chrono::steady_clock::time_point>& run_deadline = std::nullopt) {
    auto task = take_task_result(item.future);
    if (run_deadline && task.completed_at > *run_deadline &&
        (!item.step_deadline || *run_deadline <= *item.step_deadline)) {
      auto& step = value.steps[item.index];
      step.metadata["stop_reason"] = "run_timeout";
      step.metadata["cancellation_requested"] = "true";
      step.metadata["execution_detached"] = "false";
      step.metadata["result_discarded"] = "true";
      item.settled = true;
      return run_interruption::run_timeout;
    }
    if (item.step_deadline && task.completed_at > *item.step_deadline) {
      settle_timed_out_step(value, item, run_result, run_started, observations, false);
      return run_interruption::none;
    }
    auto result = std::move(task.result);
    result.metadata["elapsed_ms"] = std::to_string(elapsed_since(item.started).count());
    auto& step = value.steps[item.index];
    apply_step_result(value, step, std::move(result));
    reflect_step_result(value, step, run_result, run_started);
    observations.push_back(observe_finished_step(value, step, run_result, run_started));
    item.settled = true;
    return run_interruption::none;
  }

  static plan_step_execution_item::task_result take_task_result(
    std::future<plan_step_execution_item::task_result>& future) {
    try {
      return future.get();
    }
    catch (const std::exception& ex) {
      auto result = plan_step_result::failed(
        std::string("plan step task failed before producing a result: ") + ex.what());
      result.metadata["task_exception"] = "true";
      return {
        .result = std::move(result),
        .completed_at = std::chrono::steady_clock::now(),
      };
    }
    catch (...) {
      auto result = plan_step_result::failed("plan step task failed before producing a result");
      result.metadata["task_exception"] = "true";
      return {
        .result = std::move(result),
        .completed_at = std::chrono::steady_clock::now(),
      };
    }
  }

  void settle_timed_out_step(plan& value, plan_step_execution_item& item,
    plan_run_result& run_result, std::chrono::steady_clock::time_point run_started,
    std::vector<planning_observation>& observations, bool consume_future = true) {
    item.stop_source.request_stop();
    const bool detached = consume_future && item.future.wait_for(std::chrono::milliseconds { 0 }) !=
                                              std::future_status::ready;
    if (consume_future && !detached) {
      try {
        (void)item.future.get();
      }
      catch (...) {
      }
    }

    auto result = plan_step_result::failed("plan step timeout exceeded");
    result.metadata["stop_reason"] = "step_timeout";
    result.metadata["timeout_ms"] = std::to_string(options_.policy.step_timeout.count());
    result.metadata["elapsed_ms"] = std::to_string(elapsed_since(item.started).count());
    result.metadata["cancellation_requested"] = "true";
    result.metadata["executor_cooperative_cancellation"] =
      item.capabilities.cooperative_cancellation ? "true" : "false";
    result.metadata["execution_detached"] = detached ? "true" : "false";

    ++run_result.steps_timed_out;
    if (detached) {
      ++run_result.steps_detached;
    }

    auto& step = value.steps[item.index];
    apply_step_result(value, step, std::move(result));
    reflect_step_result(value, step, run_result, run_started);
    observations.push_back(observe_finished_step(value, step, run_result, run_started));
    item.settled = true;
  }

  void interrupt_running_steps(plan& value, std::vector<plan_step_execution_item>& running,
    plan_run_result& run_result, std::chrono::steady_clock::time_point run_started,
    std::vector<planning_observation>& observations, run_interruption interruption,
    const std::optional<std::chrono::steady_clock::time_point>& run_deadline) {
    const auto stop_reason =
      interruption == run_interruption::run_timeout ? "run_timeout" : "cancelled";
    for (auto& item : running) {
      if (item.settled) {
        continue;
      }
      if (item.future.wait_for(std::chrono::milliseconds { 0 }) == std::future_status::ready) {
        (void)settle_completed_step(value,
          item,
          run_result,
          run_started,
          observations,
          interruption == run_interruption::run_timeout ? run_deadline : std::nullopt);
        continue;
      }
      item.stop_source.request_stop();
      const bool detached =
        item.future.wait_for(std::chrono::milliseconds { 0 }) != std::future_status::ready;
      if (!detached) {
        try {
          (void)item.future.get();
        }
        catch (...) {
        }
      }
      auto& step = value.steps[item.index];
      step.metadata["stop_reason"] = stop_reason;
      step.metadata["cancellation_requested"] = "true";
      step.metadata["executor_cooperative_cancellation"] =
        item.capabilities.cooperative_cancellation ? "true" : "false";
      step.metadata["execution_detached"] = detached ? "true" : "false";
      if (!detached) {
        step.metadata["result_discarded"] = "true";
      }
      else {
        ++run_result.steps_detached;
      }
      item.settled = true;
    }
  }

  static void clear_execution_interruption_metadata(plan_step& step) {
    step.metadata.erase("stop_reason");
    step.metadata.erase("cancellation_requested");
    step.metadata.erase("executor_cooperative_cancellation");
    step.metadata.erase("execution_detached");
    step.metadata.erase("result_discarded");
  }

  planning_observation observe_finished_step(plan& value, plan_step& step,
    plan_run_result& run_result, std::chrono::steady_clock::time_point run_started) {
    if (step.status == plan_step_status::completed) {
      ++run_result.steps_completed;
    }
    else if (step.status == plan_step_status::blocked) {
      ++run_result.steps_blocked;
    }
    else if (step.status == plan_step_status::failed) {
      ++run_result.steps_failed;
    }

    const planning_observation observation {
      .plan_id = value.id,
      .step_id = step.id,
      .status = step.status,
      .output = step.output,
      .error = step.error,
      .metadata = step.metadata,
    };

    notify(step_event_for(step), &value, &step, observation);
    trace(step_event_for(step), value, &step, run_result.iterations, elapsed_since(run_started));
    remember_step(value.id, step);
    return observation;
  }

  static plan_run_stop_reason stop_reason_without_ready_step(const plan& value, bool completed) {
    if (completed) {
      return plan_run_stop_reason::completed;
    }
    if (plan_graph::has_status(value, plan_step_status::failed)) {
      return plan_run_stop_reason::failed;
    }
    if (plan_graph::has_status(value, plan_step_status::blocked)) {
      return plan_run_stop_reason::blocked;
    }
    if (plan_graph::first_pending_approval_step(value) != nullptr) {
      return plan_run_stop_reason::approval_required;
    }
    return plan_run_stop_reason::no_ready_step;
  }

  static void apply_step_result(plan& value, plan_step& step, plan_step_result result) {
    plan_step_state::apply_result(value, step, std::move(result));
  }

  void reflect_step_result(plan& value, plan_step& step, plan_run_result& run_result,
    std::chrono::steady_clock::time_point run_started) const {
    const auto reflected = plan_reflection_gate(options_.reflection).review(value, step);
    if (!reflected) {
      return;
    }

    const auto observation = plan_reflection_gate::observation_from(step, value.id);
    notify({
      .type = plan_event_type::step_reflected,
      .current_plan = &value,
      .step = &step,
      .observation = observation,
    });
    trace(plan_event_type::step_reflected,
      value,
      &step,
      run_result.iterations,
      elapsed_since(run_started));
  }

  static void hydrate_step_input(plan& value, plan_step& step) {
    plan_step_state::hydrate_input(value, step);
  }

  static std::string final_output(const plan& value) {
    return plan_step_state::final_output(value);
  }

  void validate_or_throw(const plan& value, plan_validation_options options) const {
    if (!options_.validate_plans) {
      return;
    }
    const auto validation = validate_plan(value, std::move(options));
    if (!validation.valid()) {
      throw std::runtime_error("invalid plan: " + validation.message());
    }
  }

  plan_validation_options validation_for(const planning_request& request) const {
    auto options = options_.validation;
    if (options.max_steps == 0) {
      options.max_steps = request.max_steps;
    }
    if (options.available_tools.empty()) {
      options.available_tools = request.available_tools;
    }
    if (options.available_agents.empty()) {
      options.available_agents = request.available_agents;
    }
    return options;
  }

  static plan_event_type step_event_for(const plan_step& step) {
    if (step.status == plan_step_status::completed) {
      return plan_event_type::step_completed;
    }
    if (step.status == plan_step_status::blocked) {
      return plan_event_type::step_blocked;
    }
    return plan_event_type::step_failed;
  }

  void notify(plan_event event) const {
    services().notify(std::move(event));
  }

  static std::chrono::milliseconds elapsed_since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  }

  void trace(plan_event_type type, const plan& value, const plan_step* step, std::size_t iteration,
    std::chrono::milliseconds elapsed) const {
    if (!options_.trace_sink) {
      return;
    }
    services().trace(type, value, step, iteration, elapsed);
  }

  void notify(plan_event_type type, const plan* current_plan, const plan_step* step,
    planning_observation observation) const {
    notify({
      .type = type,
      .current_plan = current_plan,
      .step = step,
      .observation = std::move(observation),
    });
  }

  void remember(std::string content, std::map<std::string, std::string> metadata) const {
    services().remember(std::move(content), std::move(metadata));
  }

  void remember_step(const std::string& plan_id, const plan_step& step) const {
    services().remember_step(plan_id, step);
  }

  void save(const plan& value) const {
    services().save(value);
  }

  plan_runtime_services services() const {
    return plan_runtime_services(
      options_.store, options_.memory, options_.observer, options_.trace_sink);
  }

  plan_step_recovery_policy recovery_policy() const {
    return plan_step_recovery_policy(options_.policy);
  }

  plan_runner_options options_;
};

} // namespace wuwe::agent::planning

#endif // WUWE_AGENT_PLANNING_PLAN_RUNNER_HPP
