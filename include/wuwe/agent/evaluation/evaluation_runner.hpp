#ifndef WUWE_AGENT_EVALUATION_EVALUATION_RUNNER_HPP
#define WUWE_AGENT_EVALUATION_EVALUATION_RUNNER_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/core/observability.hpp>
#include <wuwe/agent/evaluation/evaluation_core.hpp>

namespace wuwe::agent::evaluation {

enum class evaluator_failure_mode {
  fail_case,
  ignore,
};

struct evaluation_policy {
  double pass_threshold { 0.75 };
  bool require_all_required_checks { true };
  bool fail_fast { false };
  evaluator_failure_mode failure_mode { evaluator_failure_mode::fail_case };
};

struct evaluation_runner_options {
  evaluation_policy policy;
  evaluation_observer observer;
  observability::event_sink* event_sink {};
  observability::telemetry_failure_mode telemetry_failure_mode {
    observability::telemetry_failure_mode::ignore
  };
};

class evaluation_runner {
public:
  explicit evaluation_runner(evaluation_runner_options options = {})
      : options_(std::move(options)) {
    if (!std::isfinite(options_.policy.pass_threshold) || options_.policy.pass_threshold < 0.0 ||
        options_.policy.pass_threshold > 1.0) {
      throw std::invalid_argument(
        "evaluation pass threshold must be finite and between zero and one");
    }
  }

  evaluation_runner& add(
    std::shared_ptr<evaluator> value, double weight = 1.0, bool required = true) {
    if (!value) {
      throw std::invalid_argument("evaluation_runner cannot add a null evaluator");
    }
    if (!std::isfinite(weight) || weight < 0.0) {
      throw std::invalid_argument("evaluation evaluator weight must be finite and non-negative");
    }
    if (weight > (std::numeric_limits<double>::max)() - configured_weight_) {
      throw std::invalid_argument(
        "evaluation evaluator weights exceed the supported aggregate range");
    }
    auto name = value->name();
    if (name.empty()) {
      throw std::invalid_argument("evaluation evaluator name must not be empty");
    }
    if (std::any_of(evaluators_.begin(), evaluators_.end(), [&](const auto& entry) {
          return entry.name == name;
        })) {
      throw std::invalid_argument("duplicate evaluation evaluator name: " + name);
    }
    evaluators_.push_back({ std::move(value), std::move(name), weight, required });
    configured_weight_ += weight;
    return *this;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return evaluators_.size();
  }

  [[nodiscard]] evaluation_case_result evaluate(const evaluation_case& value) const {
    const auto started = std::chrono::steady_clock::now();
    evaluation_case_result result {
      .id = value.id,
      .metadata = value.metadata,
    };
    double weighted_score = 0.0;
    double total_weight = 0.0;
    bool required_checks_passed = true;

    for (const auto& entry : evaluators_) {
      const auto check_started = std::chrono::steady_clock::now();
      evaluation_check_result check {
        .evaluator_name = entry.name,
        .weight = entry.weight,
        .required = entry.required,
      };
      try {
        check.metric = entry.value->evaluate(value);
        check.metric.name = check.metric.name.empty() ? entry.name : check.metric.name;
        if (!std::isfinite(check.metric.score)) {
          throw std::runtime_error("evaluator returned a non-finite score");
        }
        check.metric.score = std::clamp(check.metric.score, 0.0, 1.0);
      }
      catch (const std::exception& ex) {
        check.error = ex.what();
        check.metric = failed_metric(entry.name, ex.what());
      }
      catch (...) {
        check.error = "unknown evaluator exception";
        check.metric = failed_metric(entry.name, check.error);
      }
      check.elapsed = elapsed_since(check_started);

      const bool ignored_error =
        !check.error.empty() && options_.policy.failure_mode == evaluator_failure_mode::ignore;
      if (!ignored_error) {
        weighted_score += check.metric.score * entry.weight;
        total_weight += entry.weight;
        if (entry.required && !check.metric.passed) {
          required_checks_passed = false;
        }
      }
      result.checks.push_back(std::move(check));

      if (options_.policy.fail_fast && options_.policy.require_all_required_checks &&
          !required_checks_passed) {
        break;
      }
    }

    result.score = total_weight == 0.0 ? 0.0 : weighted_score / total_weight;
    result.passed = result.score >= options_.policy.pass_threshold &&
                    (!options_.policy.require_all_required_checks || required_checks_passed);
    result.elapsed = elapsed_since(started);
    publish(result);
    return result;
  }

  [[nodiscard]] evaluation_suite_result run(const std::vector<evaluation_case>& cases) const {
    const auto started = std::chrono::steady_clock::now();
    std::vector<evaluation_case_result> results;
    results.reserve(cases.size());
    for (const auto& value : cases) {
      results.push_back(evaluate(value));
    }
    return summarize(std::move(results), started);
  }

  [[nodiscard]] evaluation_suite_result run(
    const std::vector<evaluation_case>& cases, const evaluation_target& target) const {
    if (!target) {
      throw std::invalid_argument("evaluation target is required");
    }
    const auto started = std::chrono::steady_clock::now();
    std::vector<evaluation_case_result> results;
    results.reserve(cases.size());
    for (const auto& seed : cases) {
      try {
        auto value = target(seed);
        inherit_case_defaults(value, seed);
        results.push_back(evaluate(value));
      }
      catch (const std::exception& ex) {
        results.push_back(target_failure(seed, ex.what()));
        publish(results.back());
      }
      catch (...) {
        results.push_back(target_failure(seed, "unknown evaluation target exception"));
        publish(results.back());
      }
    }
    return summarize(std::move(results), started);
  }

private:
  struct evaluator_entry {
    std::shared_ptr<evaluator> value;
    std::string name;
    double weight { 1.0 };
    bool required { true };
  };

  evaluation_suite_result summarize(std::vector<evaluation_case_result> results,
    std::chrono::steady_clock::time_point started) const {
    evaluation_suite_result suite;
    suite.total = results.size();
    suite.cases.reserve(results.size());
    std::map<std::string, evaluation_metric_summary> summaries;
    double score_sum = 0.0;

    for (auto& result : results) {
      score_sum += result.score;
      if (result.passed) {
        ++suite.passed;
      }
      else {
        ++suite.failed;
      }
      for (const auto& check : result.checks) {
        if (!check.error.empty() &&
            options_.policy.failure_mode == evaluator_failure_mode::ignore) {
          continue;
        }
        auto& summary = summaries[check.metric.name];
        summary.name = check.metric.name;
        ++summary.total;
        summary.passed += check.metric.passed ? 1U : 0U;
        summary.mean_score += check.metric.score;
      }
      suite.cases.push_back(std::move(result));
    }

    if (suite.total != 0) {
      suite.pass_rate = static_cast<double>(suite.passed) / static_cast<double>(suite.total);
      suite.mean_score = score_sum / static_cast<double>(suite.total);
    }
    for (auto& [name, summary] : summaries) {
      (void)name;
      if (summary.total != 0) {
        summary.mean_score /= static_cast<double>(summary.total);
      }
      suite.metrics.push_back(std::move(summary));
    }
    suite.elapsed = elapsed_since(started);
    return suite;
  }

  static void inherit_case_defaults(evaluation_case& value, const evaluation_case& seed) {
    if (value.id.empty())
      value.id = seed.id;
    if (value.input.empty())
      value.input = seed.input;
    if (value.expected_output.empty())
      value.expected_output = seed.expected_output;
    if (value.expected.is_null())
      value.expected = seed.expected;
    for (const auto& [key, metadata_value] : seed.metadata) {
      value.metadata.try_emplace(key, metadata_value);
    }
  }

  static evaluation_case_result target_failure(const evaluation_case& value, std::string error) {
    return {
      .id = value.id,
      .passed = false,
      .score = 0.0,
      .metadata = value.metadata,
      .error = std::move(error),
    };
  }

  static evaluation_metric_result failed_metric(std::string name, const std::string& error) {
    return {
      .name = std::move(name),
      .score = 0.0,
      .passed = false,
      .explanation = "evaluator failed: " + error,
    };
  }

  void publish(evaluation_case_result& result) const {
    std::size_t failures = 0;
    if (options_.observer) {
      failures += observability::invoke_telemetry(
                    options_.telemetry_failure_mode, [&] { options_.observer(result); })
                    ? 0U
                    : 1U;
    }
    if (options_.event_sink) {
      failures += observability::invoke_telemetry(options_.telemetry_failure_mode,
                    [&] {
                      options_.event_sink->publish({
        .module = "evaluation",
        .name = "case_evaluated",
        .subject_id = result.id,
        .elapsed = result.elapsed,
        .attributes = {
          { "passed", result.passed ? "true" : "false" },
          { "score", std::to_string(result.score) },
          { "checks", std::to_string(result.checks.size()) },
        },
      });
                    })
                    ? 0U
                    : 1U;
    }
    if (failures != 0) {
      result.metadata["telemetry_error_count"] = std::to_string(failures);
    }
  }

  static std::chrono::milliseconds elapsed_since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  }

  evaluation_runner_options options_;
  std::vector<evaluator_entry> evaluators_;
  double configured_weight_ { 0.0 };
};

} // namespace wuwe::agent::evaluation

#endif // WUWE_AGENT_EVALUATION_EVALUATION_RUNNER_HPP
