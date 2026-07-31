#ifndef WUWE_AGENT_EXPLORATION_EXPLORATION_RUNNER_HPP
#define WUWE_AGENT_EXPLORATION_EXPLORATION_RUNNER_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <wuwe/agent/approval/approval_service.hpp>
#include <wuwe/agent/core/observability.hpp>
#include <wuwe/agent/exploration/exploration_store.hpp>
#include <wuwe/agent/orchestration/fan_out.hpp>

namespace wuwe::agent::exploration {

using hypothesis_generator =
  std::function<std::vector<hypothesis>(const exploration_request&, const exploration_context&)>;
using experiment_designer = std::function<std::vector<experiment>(
  const exploration_request&, const hypothesis&, const exploration_context&)>;
using experiment_executor =
  std::function<experiment_evidence(const experiment&, const exploration_context&)>;
using evidence_reviewer = std::function<hypothesis_assessment(const exploration_request&,
  const hypothesis&, const std::vector<experiment_evidence>&, const exploration_context&)>;
using exploration_observer = std::function<void(const exploration_record&)>;

struct exploration_policy {
  std::size_t max_hypotheses { 6 };
  std::size_t max_experiments { 12 };
  std::size_t max_experiments_per_hypothesis { 3 };
  std::size_t max_concurrency { 3 };
  std::chrono::milliseconds timeout { 0 };
  double minimum_conclusion_confidence { 0.6 };
  bool allow_effectful_experiments { false };
  bool require_approval_for_effectful { true };
  bool require_approval_for_capabilities { true };
};

struct exploration_runner_options {
  hypothesis_generator generate;
  experiment_designer design;
  experiment_executor execute;
  evidence_reviewer review;
  exploration_store* store {};
  approval::approval_service* approvals {};
  exploration_observer observer;
  observability::event_sink* event_sink {};
  observability::telemetry_failure_mode telemetry_failure_mode {
    observability::telemetry_failure_mode::ignore
  };
};

struct exploration_run_options {
  exploration_policy policy;
  std::stop_token stop_token;
};

class exploration_runner {
public:
  explicit exploration_runner(exploration_runner_options options) : options_(std::move(options)) {
    if (!options_.generate || !options_.design || !options_.execute || !options_.review) {
      throw std::invalid_argument(
        "exploration_runner requires generate, design, execute, and review callbacks");
    }
  }

  [[nodiscard]] exploration_run_result run(
    exploration_request request, exploration_run_options run_options = {}) const {
    validate(run_options.policy);
    const auto started = std::chrono::steady_clock::now();
    exploration_run_result output;
    output.record.id = request.id.empty() ? make_exploration_id("exploration-run") : request.id;
    request.id = output.record.id;
    output.record.request = request;

    if (run_options.stop_token.stop_requested()) {
      stop(output,
        exploration_stop_reason::cancelled,
        "exploration cancelled before hypothesis generation");
      finalize(output, started);
      return output;
    }

    auto generated = run_generation(request, run_options.policy, run_options.stop_token, started);
    if (generated.stop_reason != fan_out_stop_reason::none || generated.items.empty() ||
        !generated.items.front()) {
      output.record.detached_count += generated.detached_count;
      stop(output,
        generated.stop_reason == fan_out_stop_reason::timed_out
          ? exploration_stop_reason::timed_out
          : (generated.stop_reason == fan_out_stop_reason::cancelled
                ? exploration_stop_reason::cancelled
                : exploration_stop_reason::generation_failed),
        generated.items.empty() || generated.items.front().error.empty()
          ? "hypothesis generation failed"
          : generated.items.front().error);
      finalize_and_publish(output, started);
      return output;
    }

    auto hypotheses = std::move(*generated.items.front().value);
    if (hypotheses.size() > run_options.policy.max_hypotheses) {
      hypotheses.resize(run_options.policy.max_hypotheses);
    }
    normalize_hypotheses(hypotheses, request);
    output.record.hypotheses.reserve(hypotheses.size());
    for (const auto& value : hypotheses) {
      output.record.hypotheses.push_back({ .value = value });
    }
    if (hypotheses.empty()) {
      output.completed = true;
      finalize_and_publish(output, started);
      return output;
    }

    auto designs =
      run_design(request, hypotheses, run_options.policy, run_options.stop_token, started);
    output.record.detached_count += designs.detached_count;
    apply_designs(output, hypotheses, designs, run_options.policy);
    if (designs.stop_reason == fan_out_stop_reason::timed_out) {
      stop(output,
        exploration_stop_reason::timed_out,
        "exploration timed out during experiment design");
      finalize_and_publish(output, started);
      return output;
    }
    if (designs.stop_reason == fan_out_stop_reason::cancelled ||
        run_options.stop_token.stop_requested()) {
      stop(output,
        exploration_stop_reason::cancelled,
        "exploration cancelled during experiment design");
      finalize_and_publish(output, started);
      return output;
    }
    if (designs.completed_count == 0 && designs.failed_count != 0) {
      stop(output, exploration_stop_reason::design_failed, "all experiment designers failed");
      finalize_and_publish(output, started);
      return output;
    }

    authorize_experiments(output.record, run_options.policy);
    auto execution_tasks = make_execution_tasks(output.record);
    if (!execution_tasks.empty()) {
      auto executions =
        run_experiments(execution_tasks, run_options.policy, run_options.stop_token, started);
      output.record.detached_count += executions.detached_count;
      apply_executions(output.record, execution_tasks, executions);
      if (executions.stop_reason == fan_out_stop_reason::timed_out) {
        stop(output,
          exploration_stop_reason::timed_out,
          "exploration timed out during experiment execution");
      }
      else if (executions.stop_reason == fan_out_stop_reason::cancelled ||
               run_options.stop_token.stop_requested()) {
        stop(output,
          exploration_stop_reason::cancelled,
          "exploration cancelled during experiment execution");
      }
    }

    if (output.record.stop_reason == exploration_stop_reason::none) {
      auto review_tasks = make_review_tasks(output.record);
      if (!review_tasks.empty()) {
        auto reviews =
          run_reviews(request, review_tasks, run_options.policy, run_options.stop_token, started);
        output.record.detached_count += reviews.detached_count;
        apply_reviews(output.record, review_tasks, reviews, run_options.policy);
        if (reviews.stop_reason == fan_out_stop_reason::timed_out) {
          stop(output,
            exploration_stop_reason::timed_out,
            "exploration timed out during evidence review");
        }
        else if (reviews.stop_reason == fan_out_stop_reason::cancelled ||
                 run_options.stop_token.stop_requested()) {
          stop(output,
            exploration_stop_reason::cancelled,
            "exploration cancelled during evidence review");
        }
        else if (reviews.completed_count == 0 && reviews.failed_count != 0) {
          stop(output, exploration_stop_reason::review_failed, "all evidence reviewers failed");
        }
      }
      mark_unreviewed_hypotheses(output.record);
    }

    output.completed = output.record.stop_reason == exploration_stop_reason::none;
    finalize_and_publish(output, started);
    return output;
  }

private:
  struct execution_task {
    std::size_t hypothesis_index { 0 };
    std::size_t experiment_index { 0 };
    experiment specification;
  };

  struct review_task {
    std::size_t hypothesis_index { 0 };
    hypothesis value;
    std::vector<experiment_evidence> evidence;
  };

  fan_out_result<std::vector<hypothesis>> run_generation(const exploration_request& request,
    const exploration_policy& policy, std::stop_token stop_token,
    std::chrono::steady_clock::time_point started) const {
    auto operation = fan_out(
      fan_out_options {
        .max_concurrency = 1,
        .failure_mode = fan_out_failure_mode::collect_all,
        .deadline = deadline_for(started, policy.timeout),
      },
      [generate = options_.generate](
        const exploration_request& value, const fan_out_context& context) {
        return generate(value,
          {
            .stop_token = context.stop_token,
            .deadline = context.deadline,
          });
      });
    return operation.run(request, stop_token);
  }

  fan_out_result<std::vector<experiment>> run_design(const exploration_request& request,
    const std::vector<hypothesis>& hypotheses, const exploration_policy& policy,
    std::stop_token stop_token, std::chrono::steady_clock::time_point started) const {
    auto operation = fan_out_each(
      fan_out_options {
        .max_concurrency = policy.max_concurrency,
        .failure_mode = fan_out_failure_mode::collect_all,
        .deadline = deadline_for(started, policy.timeout),
      },
      [design = options_.design, request](const hypothesis& value, const fan_out_context& context) {
        return design(request,
          value,
          {
            .stop_token = context.stop_token,
            .deadline = context.deadline,
          });
      });
    return operation.run(hypotheses, stop_token);
  }

  fan_out_result<experiment_evidence> run_experiments(const std::vector<execution_task>& tasks,
    const exploration_policy& policy, std::stop_token stop_token,
    std::chrono::steady_clock::time_point started) const {
    auto operation = fan_out_each(
      fan_out_options {
        .max_concurrency = policy.max_concurrency,
        .failure_mode = fan_out_failure_mode::collect_all,
        .deadline = deadline_for(started, policy.timeout),
      },
      [execute = options_.execute](const execution_task& task, const fan_out_context& context) {
        const auto operation_started = std::chrono::steady_clock::now();
        auto evidence = execute(task.specification,
          {
            .stop_token = context.stop_token,
            .deadline = context.deadline,
          });
        if (evidence.elapsed.count() == 0) {
          evidence.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - operation_started);
        }
        return evidence;
      });
    return operation.run(tasks, stop_token);
  }

  fan_out_result<hypothesis_assessment> run_reviews(const exploration_request& request,
    const std::vector<review_task>& tasks, const exploration_policy& policy,
    std::stop_token stop_token, std::chrono::steady_clock::time_point started) const {
    auto operation = fan_out_each(
      fan_out_options {
        .max_concurrency = policy.max_concurrency,
        .failure_mode = fan_out_failure_mode::collect_all,
        .deadline = deadline_for(started, policy.timeout),
      },
      [review = options_.review, request](const review_task& task, const fan_out_context& context) {
        return review(request,
          task.value,
          task.evidence,
          {
            .stop_token = context.stop_token,
            .deadline = context.deadline,
          });
      });
    return operation.run(tasks, stop_token);
  }

  static void apply_designs(exploration_run_result& output,
    const std::vector<hypothesis>& hypotheses, fan_out_result<std::vector<experiment>>& designs,
    const exploration_policy& policy) {
    std::size_t total = 0;
    for (auto& item : designs.items) {
      auto& target = output.record.hypotheses[item.index];
      if (!item) {
        target.verdict = item.status == fan_out_item_status::cancelled ? hypothesis_verdict::blocked
                                                                       : hypothesis_verdict::failed;
        target.error = item.error.empty() ? "experiment design failed" : item.error;
        continue;
      }
      auto experiments = std::move(*item.value);
      if (experiments.size() > policy.max_experiments_per_hypothesis) {
        experiments.resize(policy.max_experiments_per_hypothesis);
      }
      for (auto& specification : experiments) {
        if (total >= policy.max_experiments)
          break;
        if (specification.id.empty()) {
          specification.id = make_exploration_id("experiment");
        }
        if (specification.hypothesis_id.empty()) {
          specification.hypothesis_id = hypotheses[item.index].id;
        }
        target.experiments.push_back({ .specification = std::move(specification) });
        ++total;
      }
    }
  }

  void authorize_experiments(exploration_record& record, const exploration_policy& policy) const {
    for (auto& hypothesis : record.hypotheses) {
      for (auto& experiment : hypothesis.experiments) {
        const bool effectful = experiment.specification.safety == experiment_safety::effectful;
        if (effectful && std::none_of(experiment.specification.capabilities.begin(),
                           experiment.specification.capabilities.end(),
                           [](const capability::capability_request& request) {
                             return request.name == capability::names::exploration_execute;
                           })) {
          experiment.specification.capabilities.push_back({
            .name = capability::names::exploration_execute,
            .risk = capability::capability_risk_level::high,
            .summary = "Execute an effectful exploration experiment",
            .resources = { experiment.specification.id },
            .subject_id = experiment.specification.id,
          });
        }
        if (effectful && !policy.allow_effectful_experiments) {
          experiment.status = experiment_status::blocked;
          experiment.error = "effectful experiments are disabled by policy";
          continue;
        }
        const bool needs_approval = (effectful && policy.require_approval_for_effectful) ||
                                    (!experiment.specification.capabilities.empty() &&
                                      policy.require_approval_for_capabilities);
        if (!needs_approval) {
          experiment.status = experiment_status::approved;
          continue;
        }
        if (!options_.approvals) {
          experiment.status = experiment_status::approval_required;
          experiment.error = "experiment requires an approval service";
          continue;
        }
        try {
          experiment.approval = options_.approvals->decide({
            .id = experiment.specification.id,
            .summary = experiment.specification.title.empty() ? experiment.specification.description
                                                              : experiment.specification.title,
            .capabilities = experiment.specification.capabilities,
            .metadata = experiment.specification.metadata,
          });
          if (experiment.approval->kind == approval::approval_decision_kind::approved) {
            experiment.status = experiment_status::approved;
          }
          else {
            experiment.status =
              experiment.approval->kind == approval::approval_decision_kind::needs_manual_review
                ? experiment_status::approval_required
                : experiment_status::blocked;
            experiment.error = experiment.approval->reason;
          }
        }
        catch (const std::exception& ex) {
          experiment.status = experiment_status::approval_required;
          experiment.error = std::string("experiment approval failed: ") + ex.what();
        }
        catch (...) {
          experiment.status = experiment_status::approval_required;
          experiment.error = "experiment approval failed with an unknown exception";
        }
      }
    }
  }

  static std::vector<execution_task> make_execution_tasks(const exploration_record& record) {
    std::vector<execution_task> tasks;
    for (std::size_t hypothesis_index = 0; hypothesis_index < record.hypotheses.size();
         ++hypothesis_index) {
      const auto& hypothesis = record.hypotheses[hypothesis_index];
      for (std::size_t experiment_index = 0; experiment_index < hypothesis.experiments.size();
           ++experiment_index) {
        const auto& experiment = hypothesis.experiments[experiment_index];
        if (experiment.status == experiment_status::approved) {
          tasks.push_back({
            .hypothesis_index = hypothesis_index,
            .experiment_index = experiment_index,
            .specification = experiment.specification,
          });
        }
      }
    }
    return tasks;
  }

  static void apply_executions(exploration_record& record, const std::vector<execution_task>& tasks,
    fan_out_result<experiment_evidence>& executions) {
    for (auto& item : executions.items) {
      const auto& task = tasks[item.index];
      auto& target = record.hypotheses[task.hypothesis_index].experiments[task.experiment_index];
      if (item) {
        target.evidence = std::move(*item.value);
        target.status =
          target.evidence->succeeded ? experiment_status::completed : experiment_status::failed;
        target.error = target.evidence->error;
      }
      else {
        target.status =
          item.status == fan_out_item_status::timed_out
            ? experiment_status::timed_out
            : (item.status == fan_out_item_status::cancelled ? experiment_status::cancelled
                                                             : experiment_status::failed);
        target.error = item.error.empty() ? "experiment execution failed" : item.error;
        target.detached = item.detached;
      }
    }
  }

  static std::vector<review_task> make_review_tasks(const exploration_record& record) {
    std::vector<review_task> tasks;
    for (std::size_t index = 0; index < record.hypotheses.size(); ++index) {
      const auto& hypothesis = record.hypotheses[index];
      std::vector<experiment_evidence> evidence;
      for (const auto& experiment : hypothesis.experiments) {
        if (experiment.evidence)
          evidence.push_back(*experiment.evidence);
      }
      if (!evidence.empty()) {
        tasks.push_back({
          .hypothesis_index = index,
          .value = hypothesis.value,
          .evidence = std::move(evidence),
        });
      }
    }
    return tasks;
  }

  static void apply_reviews(exploration_record& record, const std::vector<review_task>& tasks,
    fan_out_result<hypothesis_assessment>& reviews, const exploration_policy& policy) {
    for (auto& item : reviews.items) {
      auto& target = record.hypotheses[tasks[item.index].hypothesis_index];
      if (!item) {
        target.verdict = hypothesis_verdict::failed;
        target.error = item.error.empty() ? "evidence review failed" : item.error;
        continue;
      }
      auto assessment = std::move(*item.value);
      if (!std::isfinite(assessment.confidence)) {
        target.verdict = hypothesis_verdict::failed;
        target.error = "evidence reviewer returned non-finite confidence";
        continue;
      }
      assessment.confidence = std::clamp(assessment.confidence, 0.0, 1.0);
      if (assessment.confidence < policy.minimum_conclusion_confidence &&
          assessment.verdict != hypothesis_verdict::blocked) {
        assessment.verdict = hypothesis_verdict::inconclusive;
      }
      target.verdict = assessment.verdict;
      target.assessment = std::move(assessment);
    }
  }

  static void mark_unreviewed_hypotheses(exploration_record& record) {
    for (auto& hypothesis : record.hypotheses) {
      if (hypothesis.assessment || hypothesis.verdict == hypothesis_verdict::failed) {
        continue;
      }
      const bool blocked = std::any_of(hypothesis.experiments.begin(),
        hypothesis.experiments.end(),
        [](const experiment_record& experiment) {
          return experiment.status == experiment_status::blocked ||
                 experiment.status == experiment_status::approval_required;
        });
      hypothesis.verdict = blocked ? hypothesis_verdict::blocked : hypothesis_verdict::inconclusive;
      if (hypothesis.experiments.empty()) {
        hypothesis.error = "no experiments were designed for this hypothesis";
      }
    }
  }

  static void normalize_hypotheses(
    std::vector<hypothesis>& hypotheses, const exploration_request& request) {
    for (auto& hypothesis : hypotheses) {
      if (hypothesis.id.empty())
        hypothesis.id = make_exploration_id("hypothesis");
      if (!std::isfinite(hypothesis.prior_confidence)) {
        hypothesis.prior_confidence = 0.5;
      }
      hypothesis.prior_confidence = std::clamp(hypothesis.prior_confidence, 0.0, 1.0);
      for (const auto& [key, value] : request.metadata) {
        hypothesis.metadata.try_emplace(key, value);
      }
    }
  }

  static void stop(
    exploration_run_result& output, exploration_stop_reason reason, std::string error) {
    output.record.stop_reason = reason;
    output.record.error = std::move(error);
  }

  void finalize_and_publish(
    exploration_run_result& output, std::chrono::steady_clock::time_point started) const {
    finalize(output, started);
    if (options_.store)
      options_.store->save(output.record);
    if (options_.observer && !observability::invoke_telemetry(options_.telemetry_failure_mode,
                               [&] { options_.observer(output.record); })) {
      ++output.telemetry_error_count;
    }
    if (options_.event_sink) {
      if (!observability::invoke_telemetry(options_.telemetry_failure_mode, [&] {
            options_.event_sink->publish({
        .module = "exploration",
        .name = "run_finalized",
        .subject_id = output.record.id,
        .elapsed = output.record.elapsed,
        .attributes = {
          { "stop_reason", to_string(output.record.stop_reason) },
          { "hypotheses", std::to_string(output.hypothesis_count) },
          { "experiments", std::to_string(output.experiment_count) },
          { "supported", std::to_string(output.supported_count) },
        },
      });
          })) {
        ++output.telemetry_error_count;
      }
    }
  }

  static void finalize(
    exploration_run_result& output, std::chrono::steady_clock::time_point started) {
    output.record.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
    output.record.updated_at = std::chrono::system_clock::now();
    output.hypothesis_count = output.record.hypotheses.size();
    for (const auto& hypothesis : output.record.hypotheses) {
      output.experiment_count += hypothesis.experiments.size();
      switch (hypothesis.verdict) {
        case hypothesis_verdict::supported:
          ++output.supported_count;
          break;
        case hypothesis_verdict::refuted:
          ++output.refuted_count;
          break;
        case hypothesis_verdict::inconclusive:
          ++output.inconclusive_count;
          break;
        case hypothesis_verdict::blocked:
          ++output.blocked_hypothesis_count;
          break;
        case hypothesis_verdict::failed:
          ++output.failed_hypothesis_count;
          break;
      }
      for (const auto& experiment : hypothesis.experiments) {
        switch (experiment.status) {
          case experiment_status::completed:
            ++output.completed_experiment_count;
            break;
          case experiment_status::blocked:
          case experiment_status::approval_required:
            ++output.blocked_experiment_count;
            break;
          case experiment_status::failed:
          case experiment_status::timed_out:
          case experiment_status::cancelled:
            ++output.failed_experiment_count;
            break;
          case experiment_status::proposed:
          case experiment_status::approved:
            break;
        }
      }
    }
  }

  static void validate(const exploration_policy& policy) {
    if (policy.max_hypotheses == 0 || policy.max_experiments == 0 ||
        policy.max_experiments_per_hypothesis == 0 || policy.max_concurrency == 0) {
      throw std::invalid_argument("exploration limits must be greater than zero");
    }
    if (policy.timeout.count() < 0) {
      throw std::invalid_argument("exploration timeout must not be negative");
    }
    if (!std::isfinite(policy.minimum_conclusion_confidence) ||
        policy.minimum_conclusion_confidence < 0.0 || policy.minimum_conclusion_confidence > 1.0) {
      throw std::invalid_argument(
        "exploration minimum conclusion confidence must be within [0, 1]");
    }
  }

  static std::optional<std::chrono::steady_clock::time_point> deadline_for(
    std::chrono::steady_clock::time_point started, std::chrono::milliseconds timeout) noexcept {
    return timeout.count() == 0 ? std::optional<std::chrono::steady_clock::time_point> {}
                                : std::optional(started + timeout);
  }

  exploration_runner_options options_;
};

} // namespace wuwe::agent::exploration

#endif // WUWE_AGENT_EXPLORATION_EXPLORATION_RUNNER_HPP
