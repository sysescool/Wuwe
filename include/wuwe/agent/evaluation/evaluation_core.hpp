#ifndef WUWE_AGENT_EVALUATION_EVALUATION_CORE_HPP
#define WUWE_AGENT_EVALUATION_EVALUATION_CORE_HPP

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace wuwe::agent::evaluation {

struct evaluation_case {
  std::string id;
  std::string input;
  std::string output;
  std::string expected_output;
  nlohmann::json actual;
  nlohmann::json expected;
  nlohmann::json trajectory;
  std::chrono::milliseconds elapsed { 0 };
  std::map<std::string, std::string> metadata;
};

using evaluation_target = std::function<evaluation_case(const evaluation_case&)>;

struct evaluation_metric_result {
  std::string name;
  double score { 0.0 };
  bool passed { false };
  std::string explanation;
  std::vector<std::string> evidence;
  std::map<std::string, std::string> metadata;
};

class evaluator {
public:
  virtual ~evaluator() = default;

  [[nodiscard]] virtual std::string name() const = 0;
  virtual evaluation_metric_result evaluate(const evaluation_case& value) const = 0;
};

class function_evaluator final : public evaluator {
public:
  using callback = std::function<evaluation_metric_result(const evaluation_case&)>;

  function_evaluator(std::string name, callback evaluate)
      : name_(std::move(name)), evaluate_(std::move(evaluate)) {
    if (name_.empty()) {
      throw std::invalid_argument("function_evaluator requires a name");
    }
    if (!evaluate_) {
      throw std::invalid_argument("function_evaluator requires a callback");
    }
  }

  [[nodiscard]] std::string name() const override {
    return name_;
  }

  evaluation_metric_result evaluate(const evaluation_case& value) const override {
    auto result = evaluate_(value);
    if (result.name.empty()) {
      result.name = name_;
    }
    return result;
  }

private:
  std::string name_;
  callback evaluate_;
};

struct evaluation_check_result {
  std::string evaluator_name;
  evaluation_metric_result metric;
  double weight { 1.0 };
  bool required { true };
  std::chrono::milliseconds elapsed { 0 };
  std::string error;
};

struct evaluation_case_result {
  std::string id;
  bool passed { false };
  double score { 0.0 };
  std::vector<evaluation_check_result> checks;
  std::map<std::string, std::string> metadata;
  std::chrono::milliseconds elapsed { 0 };
  std::string error;
};

struct evaluation_metric_summary {
  std::string name;
  std::size_t total { 0 };
  std::size_t passed { 0 };
  double mean_score { 0.0 };
};

struct evaluation_suite_result {
  std::size_t total { 0 };
  std::size_t passed { 0 };
  std::size_t failed { 0 };
  double pass_rate { 0.0 };
  double mean_score { 0.0 };
  std::vector<evaluation_metric_summary> metrics;
  std::vector<evaluation_case_result> cases;
  std::chrono::milliseconds elapsed { 0 };

  [[nodiscard]] bool successful() const noexcept {
    return total != 0 && failed == 0;
  }
};

using evaluation_observer = std::function<void(const evaluation_case_result&)>;

inline nlohmann::json evaluation_metric_result_to_json(const evaluation_metric_result& result) {
  return {
    { "name", result.name },
    { "score", result.score },
    { "passed", result.passed },
    { "explanation", result.explanation },
    { "evidence", result.evidence },
    { "metadata", result.metadata },
  };
}

inline nlohmann::json evaluation_suite_result_to_json(const evaluation_suite_result& result) {
  auto metrics = nlohmann::json::array();
  for (const auto& metric : result.metrics) {
    metrics.push_back({
      { "name", metric.name },
      { "total", metric.total },
      { "passed", metric.passed },
      { "mean_score", metric.mean_score },
    });
  }
  auto cases = nlohmann::json::array();
  for (const auto& item : result.cases) {
    auto checks = nlohmann::json::array();
    for (const auto& check : item.checks) {
      checks.push_back({
        { "evaluator", check.evaluator_name },
        { "metric", evaluation_metric_result_to_json(check.metric) },
        { "weight", check.weight },
        { "required", check.required },
        { "elapsed_ms", check.elapsed.count() },
        { "error", check.error },
      });
    }
    cases.push_back({
      { "id", item.id },
      { "passed", item.passed },
      { "score", item.score },
      { "checks", std::move(checks) },
      { "metadata", item.metadata },
      { "elapsed_ms", item.elapsed.count() },
      { "error", item.error },
    });
  }
  return {
    { "total", result.total },
    { "passed", result.passed },
    { "failed", result.failed },
    { "pass_rate", result.pass_rate },
    { "mean_score", result.mean_score },
    { "metrics", std::move(metrics) },
    { "cases", std::move(cases) },
    { "elapsed_ms", result.elapsed.count() },
  };
}

} // namespace wuwe::agent::evaluation

#endif // WUWE_AGENT_EVALUATION_EVALUATION_CORE_HPP
