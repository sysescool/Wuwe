#ifndef WUWE_AGENT_LEARNING_LEARNING_RUNNER_HPP
#define WUWE_AGENT_LEARNING_LEARNING_RUNNER_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include <wuwe/agent/approval/approval_service.hpp>
#include <wuwe/agent/capability/capability.hpp>
#include <wuwe/agent/core/observability.hpp>
#include <wuwe/agent/learning/learning_store.hpp>
#include <wuwe/agent/orchestration/fan_out.hpp>

namespace wuwe::agent::learning {

using learning_proposer = std::function<std::vector<learning_candidate>(
  const learning_request&, const learning_context&)>;
using learning_evaluator = std::function<learning_evaluation(
  const learning_candidate&, const learning_context&)>;
using learning_activator = std::function<learning_activation_result(
  const learning_candidate&, const learning_context&)>;
using learning_candidate_selector = std::function<std::optional<std::size_t>(
  const std::vector<learning_record>&,
  const std::vector<std::size_t>&)>;
using learning_observer = std::function<void(const learning_record&)>;

struct learning_policy {
  std::size_t max_candidates { 8 };
  std::size_t max_concurrency { 2 };
  std::chrono::milliseconds timeout { 0 };
  double minimum_candidate_score { 0.75 };
  double minimum_candidate_pass_rate { 0.75 };
  double minimum_score_improvement { 0.0 };
  double minimum_pass_rate_improvement { 0.0 };
  std::size_t maximum_regressions { 0 };
  learning_activation_mode activation_mode {
    learning_activation_mode::stage_only
  };
  bool persist_rejected { true };
};

struct learning_runner_options {
  learning_proposer proposer;
  learning_evaluator evaluator;
  learning_activator activator;
  learning_candidate_selector selector;
  learning_store* store {};
  approval::approval_service* approvals {};
  learning_observer observer;
  observability::event_sink* event_sink {};
  observability::telemetry_failure_mode telemetry_failure_mode {
    observability::telemetry_failure_mode::ignore
  };
};

struct learning_run_options {
  learning_policy policy;
  std::stop_token stop_token;
};

class learning_runner {
public:
  explicit learning_runner(learning_runner_options options)
      : options_(std::move(options)) {
    if (!options_.proposer) {
      throw std::invalid_argument("learning_runner requires a proposer");
    }
    if (!options_.evaluator) {
      throw std::invalid_argument("learning_runner requires an evaluator");
    }
  }

  [[nodiscard]] learning_run_result run(
    learning_request request,
    learning_run_options run_options = {}) const {
    validate(run_options.policy);
    if (run_options.policy.activation_mode != learning_activation_mode::stage_only &&
        !options_.activator) {
      throw std::invalid_argument(
        "learning activation mode requires an activator callback");
    }

    const auto started = std::chrono::steady_clock::now();
    learning_run_result output;
    output.run_id = request.id.empty() ? make_learning_id("learning-run") : request.id;
    request.id = output.run_id;

    if (run_options.stop_token.stop_requested()) {
      output.stop_reason = learning_stop_reason::cancelled;
      output.error = "learning run cancelled before proposal";
      finalize(output, started);
      return output;
    }

    auto proposed = run_proposer(
      request, run_options.policy, run_options.stop_token, started);
    if (proposed.stop_reason != fan_out_stop_reason::none ||
        proposed.items.empty() || !proposed.items.front()) {
      output.stop_reason = proposed.stop_reason == fan_out_stop_reason::timed_out
                             ? learning_stop_reason::timed_out
                             : (proposed.stop_reason == fan_out_stop_reason::cancelled
                                  ? learning_stop_reason::cancelled
                                  : learning_stop_reason::proposal_failed);
      output.error = proposed.items.empty() || proposed.items.front().error.empty()
                       ? "learning proposal failed"
                       : proposed.items.front().error;
      output.detached_count = proposed.detached_count;
      finalize(output, started);
      return output;
    }

    auto candidates = std::move(*proposed.items.front().value);
    output.proposed_count = candidates.size();
    if (candidates.size() > run_options.policy.max_candidates) {
      output.truncated_count = candidates.size() - run_options.policy.max_candidates;
      candidates.resize(run_options.policy.max_candidates);
    }
    normalize_candidates(candidates, request);
    if (candidates.empty()) {
      output.completed = true;
      finalize(output, started);
      return output;
    }

    const auto evaluation_timeout = remaining_time(
      started, run_options.policy.timeout);
    const auto candidate_snapshot = candidates;
    auto evaluate = fan_out_each(
      fan_out_options {
        .max_concurrency = run_options.policy.max_concurrency,
        .failure_mode = fan_out_failure_mode::collect_all,
        .timeout = evaluation_timeout,
      },
      [evaluator = options_.evaluator, run_id = output.run_id](
        const learning_candidate& candidate,
        const fan_out_context& context) {
        learning_record record {
          .id = make_learning_id("learning-record"),
          .run_id = run_id,
          .candidate = candidate,
        };
        record.evaluation = evaluator(candidate, {
          .stop_token = context.stop_token,
          .deadline = context.deadline,
        });
        return record;
      });
    auto evaluated = evaluate.run(std::move(candidates), run_options.stop_token);
    output.detached_count += evaluated.detached_count;
    output.records.reserve(evaluated.items.size());
    for (auto& item : evaluated.items) {
      if (item) {
        auto record = std::move(*item.value);
        apply_evaluation_policy(record, run_options.policy);
        output.records.push_back(std::move(record));
        continue;
      }
      learning_record record {
        .id = make_learning_id("learning-record"),
        .run_id = output.run_id,
        .candidate = candidate_snapshot[item.index],
        .status = item.status == fan_out_item_status::timed_out
                    ? learning_candidate_status::timed_out
                    : (item.status == fan_out_item_status::cancelled
                         ? learning_candidate_status::cancelled
                         : learning_candidate_status::evaluation_failed),
        .error = item.error.empty() ? "learning evaluation failed" : item.error,
        .detached = item.detached,
      };
      output.records.push_back(std::move(record));
    }

    if (evaluated.stop_reason == fan_out_stop_reason::timed_out) {
      output.stop_reason = learning_stop_reason::timed_out;
      output.error = "learning run timed out during evaluation";
    }
    else if (evaluated.stop_reason == fan_out_stop_reason::cancelled ||
             run_options.stop_token.stop_requested()) {
      output.stop_reason = learning_stop_reason::cancelled;
      output.error = "learning run cancelled during evaluation";
    }
    else {
      activate_candidates(output, run_options, started);
    }

    output.telemetry_error_count =
      persist_and_publish(output.records, run_options.policy);
    output.completed = output.stop_reason == learning_stop_reason::none;
    summarize(output);
    finalize(output, started);
    return output;
  }

private:
  fan_out_result<std::vector<learning_candidate>> run_proposer(
    const learning_request& request,
    const learning_policy& policy,
    std::stop_token stop_token,
    std::chrono::steady_clock::time_point started) const {
    auto operation = fan_out(
      fan_out_options {
        .max_concurrency = 1,
        .failure_mode = fan_out_failure_mode::collect_all,
        .timeout = remaining_time(started, policy.timeout),
      },
      [proposer = options_.proposer](
        const learning_request& value,
        const fan_out_context& context) {
        return proposer(value, {
          .stop_token = context.stop_token,
          .deadline = context.deadline,
        });
      });
    return operation.run(request, stop_token);
  }

  void activate_candidates(
    learning_run_result& output,
    const learning_run_options& run_options,
    std::chrono::steady_clock::time_point started) const {
    if (run_options.policy.activation_mode == learning_activation_mode::stage_only) {
      return;
    }
    select_activation_candidates(output.records);
    for (auto& record : output.records) {
      if (record.status != learning_candidate_status::accepted) continue;
      if (run_options.stop_token.stop_requested()) {
        record.status = learning_candidate_status::cancelled;
        record.error = "learning activation cancelled";
        output.stop_reason = learning_stop_reason::cancelled;
        output.error = record.error;
        return;
      }
      if (run_options.policy.activation_mode ==
          learning_activation_mode::require_approval) {
        if (!options_.approvals) {
          record.status = learning_candidate_status::approval_required;
          record.error = "learning activation requires an approval service";
          continue;
        }
        try {
          record.approval = options_.approvals->decide({
            .id = record.id,
            .summary = "Activate learned artifact " +
                       record.candidate.proposed_version + " for " +
                       record.candidate.target,
            .capabilities = {
              {
                .name = capability::names::learning_activate,
                .risk = capability::capability_risk_level::high,
                .summary = "Activate a versioned learned artifact",
                .resources = { record.candidate.target },
                .subject_id = record.id,
              },
            },
            .metadata = record.candidate.metadata,
          });
        }
        catch (const std::exception& ex) {
          record.status = learning_candidate_status::approval_required;
          record.error = std::string("learning approval failed: ") + ex.what();
          continue;
        }
        if (record.approval->kind != approval::approval_decision_kind::approved) {
          record.status = record.approval->kind ==
                              approval::approval_decision_kind::needs_manual_review
                            ? learning_candidate_status::approval_required
                            : learning_candidate_status::approval_denied;
          record.error = record.approval->reason;
          continue;
        }
      }

      const auto timeout = remaining_time(started, run_options.policy.timeout);
      if (run_options.policy.timeout.count() > 0 && timeout.count() == 0) {
        record.status = learning_candidate_status::timed_out;
        record.error = "learning activation timed out before start";
        output.stop_reason = learning_stop_reason::timed_out;
        output.error = record.error;
        return;
      }
      auto activate = fan_out(
        fan_out_options {
          .max_concurrency = 1,
          .failure_mode = fan_out_failure_mode::collect_all,
          .timeout = timeout,
        },
        [activator = options_.activator](
          const learning_candidate& candidate,
          const fan_out_context& context) {
          return activator(candidate, {
            .stop_token = context.stop_token,
            .deadline = context.deadline,
          });
        });
      auto activated = activate.run(record.candidate, run_options.stop_token);
      auto& item = activated.items.front();
      if (!item) {
        record.status = item.status == fan_out_item_status::timed_out
                          ? learning_candidate_status::timed_out
                          : (item.status == fan_out_item_status::cancelled
                               ? learning_candidate_status::cancelled
                               : learning_candidate_status::activation_failed);
        record.error = item.error.empty() ? "learning activation failed" : item.error;
        record.detached = item.detached;
        output.detached_count += item.detached ? 1U : 0U;
        if (item.status == fan_out_item_status::timed_out) {
          output.stop_reason = learning_stop_reason::timed_out;
          output.error = record.error;
          return;
        }
        if (item.status == fan_out_item_status::cancelled) {
          output.stop_reason = learning_stop_reason::cancelled;
          output.error = record.error;
          return;
        }
        continue;
      }
      record.activation = std::move(*item.value);
      record.status = record.activation->activated
                        ? learning_candidate_status::activated
                        : learning_candidate_status::activation_failed;
      record.error = record.activation->error;
      record.updated_at = std::chrono::system_clock::now();
    }
  }

  void select_activation_candidates(std::vector<learning_record>& records) const {
    std::map<std::string, std::vector<std::size_t>> eligible_by_target;
    for (std::size_t index = 0; index < records.size(); ++index) {
      if (records[index].status == learning_candidate_status::accepted) {
        eligible_by_target[records[index].candidate.target].push_back(index);
      }
    }

    for (const auto& [target, eligible] : eligible_by_target) {
      std::optional<std::size_t> selected;
      try {
        selected = options_.selector
                     ? options_.selector(records, eligible)
                     : select_best_candidate(records, eligible);
      }
      catch (const std::exception& ex) {
        mark_selection_failed(records, eligible,
          std::string("learning candidate selection failed: ") + ex.what());
        continue;
      }
      catch (...) {
        mark_selection_failed(records, eligible,
          "learning candidate selection failed with an unknown exception");
        continue;
      }

      if (!selected || std::find(eligible.begin(), eligible.end(), *selected) == eligible.end()) {
        mark_selection_failed(records, eligible,
          "learning candidate selector did not return an eligible candidate for target " +
          target);
        continue;
      }
      for (const auto index : eligible) {
        if (index == *selected) continue;
        records[index].status = learning_candidate_status::not_selected;
        records[index].error =
          "another accepted candidate was selected for target " + target;
        records[index].updated_at = std::chrono::system_clock::now();
      }
    }
  }

  static std::optional<std::size_t> select_best_candidate(
    const std::vector<learning_record>& records,
    const std::vector<std::size_t>& eligible) {
    if (eligible.empty()) return std::nullopt;
    return *std::max_element(
      eligible.begin(), eligible.end(),
      [&](std::size_t lhs, std::size_t rhs) {
        const auto& left = *records[lhs].evaluation;
        const auto& right = *records[rhs].evaluation;
        return std::make_tuple(
                 left.candidate_score,
                 left.candidate_pass_rate,
                 left.score_improvement(),
                 left.pass_rate_improvement(),
                 (std::numeric_limits<std::size_t>::max)() - left.regression_count) <
               std::make_tuple(
                 right.candidate_score,
                 right.candidate_pass_rate,
                 right.score_improvement(),
                 right.pass_rate_improvement(),
                 (std::numeric_limits<std::size_t>::max)() - right.regression_count);
      });
  }

  static void mark_selection_failed(
    std::vector<learning_record>& records,
    const std::vector<std::size_t>& eligible,
    const std::string& error) {
    for (const auto index : eligible) {
      records[index].status = learning_candidate_status::activation_failed;
      records[index].error = error;
      records[index].updated_at = std::chrono::system_clock::now();
    }
  }

  static void normalize_candidates(
    std::vector<learning_candidate>& candidates,
    const learning_request& request) {
    for (auto& candidate : candidates) {
      if (candidate.id.empty()) candidate.id = make_learning_id("learning-candidate");
      if (candidate.target.empty()) candidate.target = request.target;
      if (candidate.parent_version.empty()) {
        candidate.parent_version = request.baseline_version;
      }
      if (candidate.proposed_version.empty()) {
        candidate.proposed_version = candidate.id;
      }
      for (const auto& [key, value] : request.metadata) {
        candidate.metadata.try_emplace(key, value);
      }
    }
  }

  static void apply_evaluation_policy(
    learning_record& record,
    const learning_policy& policy) {
    if (!record.evaluation || !valid_evaluation(*record.evaluation)) {
      record.status = learning_candidate_status::evaluation_failed;
      record.error = "learning evaluator returned invalid metrics";
      return;
    }
    const auto& value = *record.evaluation;
    const bool accepted = value.passed &&
      value.candidate_score >= policy.minimum_candidate_score &&
      value.candidate_pass_rate >= policy.minimum_candidate_pass_rate &&
      value.score_improvement() >= policy.minimum_score_improvement &&
      value.pass_rate_improvement() >= policy.minimum_pass_rate_improvement &&
      value.regression_count <= policy.maximum_regressions;
    record.status = accepted ? learning_candidate_status::accepted
                             : learning_candidate_status::rejected;
    if (!accepted) record.error = "learning candidate did not pass promotion policy";
    record.updated_at = std::chrono::system_clock::now();
  }

  static bool valid_evaluation(const learning_evaluation& value) noexcept {
    const auto valid_unit = [](double metric) {
      return std::isfinite(metric) && metric >= 0.0 && metric <= 1.0;
    };
    return valid_unit(value.baseline_score) &&
           valid_unit(value.candidate_score) &&
           valid_unit(value.baseline_pass_rate) &&
           valid_unit(value.candidate_pass_rate);
  }

  std::size_t persist_and_publish(
    const std::vector<learning_record>& records,
    const learning_policy& policy) const {
    std::size_t failures = 0;
    for (const auto& record : records) {
      const bool rejected = record.status == learning_candidate_status::rejected;
      if (options_.store && (!rejected || policy.persist_rejected)) {
        options_.store->save(record);
      }
      if (options_.observer && !observability::invoke_telemetry(
          options_.telemetry_failure_mode,
          [&] { options_.observer(record); })) {
        ++failures;
      }
      if (options_.event_sink) {
        if (!observability::invoke_telemetry(
            options_.telemetry_failure_mode,
            [&] { options_.event_sink->publish({
          .module = "learning",
          .name = "candidate_finalized",
          .subject_id = record.id,
          .attributes = {
            { "run_id", record.run_id },
            { "status", to_string(record.status) },
            { "target", record.candidate.target },
            { "version", record.candidate.proposed_version },
          },
        }); })) {
          ++failures;
        }
      }
    }
    return failures;
  }

  static void summarize(learning_run_result& output) {
    for (const auto& record : output.records) {
      if (record.evaluation) ++output.evaluated_count;
      switch (record.status) {
        case learning_candidate_status::accepted: ++output.accepted_count; break;
        case learning_candidate_status::not_selected:
          ++output.accepted_count;
          break;
        case learning_candidate_status::rejected: ++output.rejected_count; break;
        case learning_candidate_status::approval_required:
          ++output.accepted_count;
          ++output.approval_required_count;
          break;
        case learning_candidate_status::approval_denied:
          ++output.accepted_count;
          ++output.approval_denied_count;
          break;
        case learning_candidate_status::activated:
          ++output.accepted_count;
          ++output.activated_count;
          break;
        case learning_candidate_status::activation_failed:
          ++output.accepted_count;
          ++output.failed_count;
          break;
        case learning_candidate_status::evaluation_failed:
        case learning_candidate_status::timed_out:
        case learning_candidate_status::cancelled:
          ++output.failed_count;
          break;
        case learning_candidate_status::proposed: break;
      }
    }
  }

  static void validate(const learning_policy& policy) {
    if (policy.max_candidates == 0) {
      throw std::invalid_argument("learning max_candidates must be greater than zero");
    }
    if (policy.max_concurrency == 0) {
      throw std::invalid_argument("learning max_concurrency must be greater than zero");
    }
    if (policy.timeout.count() < 0) {
      throw std::invalid_argument("learning timeout must not be negative");
    }
    for (const auto value : {
           policy.minimum_candidate_score,
           policy.minimum_candidate_pass_rate,
           policy.minimum_score_improvement,
           policy.minimum_pass_rate_improvement }) {
      if (!std::isfinite(value)) {
        throw std::invalid_argument("learning thresholds must be finite");
      }
    }
    if (policy.minimum_candidate_score < 0.0 ||
        policy.minimum_candidate_score > 1.0 ||
        policy.minimum_candidate_pass_rate < 0.0 ||
        policy.minimum_candidate_pass_rate > 1.0) {
      throw std::invalid_argument("learning absolute thresholds must be within [0, 1]");
    }
  }

  static std::chrono::milliseconds remaining_time(
    std::chrono::steady_clock::time_point started,
    std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() == 0) return std::chrono::milliseconds { 0 };
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
    return elapsed >= timeout ? std::chrono::milliseconds { 0 }
                              : timeout - elapsed;
  }

  static void finalize(
    learning_run_result& output,
    std::chrono::steady_clock::time_point started) noexcept {
    output.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  }

  learning_runner_options options_;
};

} // namespace wuwe::agent::learning

#endif // WUWE_AGENT_LEARNING_LEARNING_RUNNER_HPP
