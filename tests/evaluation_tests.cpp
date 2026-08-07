#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <wuwe/agent/evaluation/evaluation.hpp>
#include <wuwe/agent/evaluation/reasoning_evaluation.hpp>

using namespace wuwe::agent::evaluation;

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class custom_named_evaluator final : public evaluator {
public:
  explicit custom_named_evaluator(std::string value) : name_(std::move(value)) {
  }

  std::string name() const override {
    ++name_calls;
    return name_;
  }

  evaluation_metric_result evaluate(const evaluation_case&) const override {
    return {
      .score = 1.0,
      .passed = true,
    };
  }

  mutable std::size_t name_calls { 0 };

private:
  std::string name_;
};

void runner_aggregates_weighted_metrics_and_suites() {
  evaluation_runner runner({
    .policy = { .pass_threshold = 0.75 },
  });
  runner.add(std::make_shared<exact_match_evaluator>(), 2.0, true);
  runner.add(std::make_shared<contains_terms_evaluator>(contains_terms_options {
               .terms = { "hello", "world" },
             }),
    1.0,
    true);

  const auto suite = runner.run({
    { .id = "pass", .output = "Hello World", .expected_output = "hello world" },
    { .id = "fail", .output = "Hello", .expected_output = "different" },
  });

  require(suite.total == 2 && suite.passed == 1 && suite.failed == 1,
    "evaluation suite aggregates case outcomes");
  require(suite.metrics.size() == 2, "evaluation suite aggregates named metrics");
  require(suite.cases.front().passed, "passing case satisfies all required checks");
  require(!suite.cases.back().passed, "failing required checks fail the case");
  require(evaluation_suite_result_to_json(suite).at("cases").size() == 2,
    "evaluation suite has a stable JSON representation");
}

void trajectory_evaluator_checks_required_forbidden_and_budgets() {
  trajectory_evaluator evaluator({
    .required_events = { "model_started", "tool_completed" },
    .required_sequence = { "model_started", "tool_started", "tool_completed" },
    .forbidden_events = { "failed" },
    .maximum_occurrences = { { "tool_started", 1 } },
  });
  const auto passed = evaluator.evaluate({
    .id = "trajectory-pass",
    .trajectory = nlohmann::json::array({
      { { "type", "model_started" } },
      { { "type", "tool_started" } },
      { { "type", "tool_completed" } },
    }),
  });
  require(
    passed.passed && passed.score == 1.0, "trajectory evaluator accepts a compliant trajectory");

  const auto failed = evaluator.evaluate({
    .id = "trajectory-fail",
    .trajectory = nlohmann::json::array({ "model_started", "failed" }),
  });
  require(!failed.passed, "trajectory evaluator rejects violations");
  require(!failed.evidence.empty(), "trajectory evaluator reports regression evidence");
}

void runner_can_execute_an_evaluation_target() {
  evaluation_runner runner;
  runner.add(std::make_shared<exact_match_evaluator>());
  const auto suite = runner.run(
    {
      { .id = "target", .input = "question", .expected_output = "answer" },
    },
    [](const evaluation_case& seed) {
      return evaluation_case {
        .id = seed.id,
        .input = seed.input,
        .output = "answer",
      };
    });
  require(suite.successful(), "evaluation runner can execute and score a target");
  require(
    suite.cases.front().checks.size() == 1, "target output is evaluated by configured evaluators");

  const auto failed =
    runner.run({ { .id = "target-error" } }, [](const evaluation_case&) -> evaluation_case {
      throw std::runtime_error("target unavailable");
    });
  require(failed.failed == 1 && failed.cases.front().error == "target unavailable",
    "target exceptions are isolated as failed cases");
}

void evaluator_failures_are_isolated_by_policy() {
  auto throwing = std::make_shared<function_evaluator>(
    "throwing", [](const evaluation_case&) -> evaluation_metric_result {
      throw std::runtime_error("evaluator unavailable");
    });

  evaluation_runner closed;
  closed.add(throwing);
  const auto failed = closed.evaluate({ .id = "closed" });
  require(!failed.passed, "evaluator failures fail the case by default");
  require(failed.checks.front().error == "evaluator unavailable",
    "evaluator failures preserve diagnostics");

  evaluation_runner open({
    .policy = {
      .pass_threshold = 0.0,
      .failure_mode = evaluator_failure_mode::ignore,
    },
  });
  open.add(throwing);
  const auto ignored = open.evaluate({ .id = "open" });
  require(ignored.passed, "ignored evaluator failures do not fail a zero-threshold case");
}

void reasoning_adapter_preserves_trace_and_usage() {
  wuwe::agent::reasoning::reasoning_result result;
  result.completed = true;
  result.content = "answer";
  result.elapsed = std::chrono::milliseconds(12);
  result.trace.push_back({
    .sequence = 1,
    .type = wuwe::agent::reasoning::reasoning_event_type::model_started,
  });

  const auto value = evaluation_case_from_reasoning("reasoning-case", "question", result, "answer");
  require(value.output == "answer" && value.expected_output == "answer",
    "reasoning adapter maps output expectations");
  require(value.trajectory.size() == 1, "reasoning adapter exposes the trajectory");
  require(
    value.actual.at("completed").get<bool>(), "reasoning adapter exposes structured result data");
}

void fail_fast_respects_optional_required_checks_policy() {
  evaluation_runner runner({
    .policy = {
      .pass_threshold = 0.5,
      .require_all_required_checks = false,
      .fail_fast = true,
    },
  });
  runner.add(std::make_shared<function_evaluator>("first", [](const evaluation_case&) {
    return evaluation_metric_result {
      .name = "first",
      .score = 0.0,
      .passed = false,
    };
  }));
  runner.add(std::make_shared<function_evaluator>("second", [](const evaluation_case&) {
    return evaluation_metric_result {
      .name = "second",
      .score = 1.0,
      .passed = true,
    };
  }));

  const auto result = runner.evaluate({ .id = "optional-required-checks" });
  require(result.checks.size() == 2,
    "fail-fast does not stop on a failed required check when all checks are optional");
  require(result.passed && result.score == 0.5,
    "all executed evaluators contribute to threshold-based evaluation");
}

void rejects_non_finite_values_and_controls_telemetry_failures() {
  bool invalid_threshold = false;
  try {
    evaluation_runner invalid({
      .policy = { .pass_threshold = std::numeric_limits<double>::quiet_NaN() },
    });
    (void)invalid;
  }
  catch (const std::invalid_argument&) {
    invalid_threshold = true;
  }

  evaluation_runner runner({
    .observer =
      [](const evaluation_case_result&) { throw std::runtime_error("observer unavailable"); },
  });
  bool invalid_weight = false;
  try {
    runner.add(std::make_shared<function_evaluator>(
                 "weight", [](const evaluation_case&) { return evaluation_metric_result {}; }),
      std::numeric_limits<double>::infinity());
  }
  catch (const std::invalid_argument&) {
    invalid_weight = true;
  }
  runner.add(std::make_shared<function_evaluator>("non-finite", [](const evaluation_case&) {
    return evaluation_metric_result {
      .score = std::numeric_limits<double>::quiet_NaN(),
      .passed = true,
    };
  }));
  const auto result = runner.evaluate({ .id = "finite" });
  require(invalid_threshold && invalid_weight && !result.passed &&
            result.checks.front().error.find("non-finite") != std::string::npos &&
            result.metadata.at("telemetry_error_count") == "1",
    "evaluation rejects non-finite configuration and isolates telemetry by default");

  evaluation_runner strict({
    .observer =
      [](const evaluation_case_result&) { throw std::runtime_error("observer unavailable"); },
    .telemetry_failure_mode = wuwe::agent::observability::telemetry_failure_mode::propagate,
  });
  strict.add(std::make_shared<function_evaluator>("ok", [](const evaluation_case&) {
    return evaluation_metric_result { .score = 1.0, .passed = true };
  }));
  bool propagated = false;
  try {
    (void)strict.evaluate({ .id = "strict" });
  }
  catch (const std::runtime_error&) {
    propagated = true;
  }
  require(propagated, "evaluation telemetry can be configured to propagate");

  bool empty_callback = false;
  try {
    (void)function_evaluator("empty", {});
  }
  catch (const std::invalid_argument&) {
    empty_callback = true;
  }
  require(empty_callback, "function evaluators reject empty callbacks at construction");
}

void evaluator_registration_has_stable_identity_and_bounded_weights() {
  evaluation_runner runner;
  auto stable = std::make_shared<custom_named_evaluator>("stable");
  runner.add(stable, (std::numeric_limits<double>::max)());
  const auto result = runner.evaluate({ .id = "stable-name" });
  require(
    result.passed && result.checks.front().evaluator_name == "stable" && stable->name_calls == 1,
    "evaluation validates and freezes evaluator identity at registration");

  bool aggregate_overflow = false;
  try {
    runner.add(std::make_shared<custom_named_evaluator>("overflow"), 1.0);
  }
  catch (const std::invalid_argument&) {
    aggregate_overflow = true;
  }

  bool duplicate_name = false;
  try {
    evaluation_runner duplicate;
    duplicate.add(std::make_shared<custom_named_evaluator>("same"));
    duplicate.add(std::make_shared<custom_named_evaluator>("same"));
  }
  catch (const std::invalid_argument&) {
    duplicate_name = true;
  }

  bool empty_name = false;
  try {
    evaluation_runner empty;
    empty.add(std::make_shared<custom_named_evaluator>(""));
  }
  catch (const std::invalid_argument&) {
    empty_name = true;
  }
  require(aggregate_overflow && duplicate_name && empty_name,
    "evaluation rejects aggregate weight overflow and ambiguous evaluator identities");
}

} // namespace

int main() {
  runner_aggregates_weighted_metrics_and_suites();
  trajectory_evaluator_checks_required_forbidden_and_budgets();
  evaluator_failures_are_isolated_by_policy();
  runner_can_execute_an_evaluation_target();
  reasoning_adapter_preserves_trace_and_usage();
  fail_fast_respects_optional_required_checks_policy();
  rejects_non_finite_values_and_controls_telemetry_failures();
  evaluator_registration_has_stable_identity_and_bounded_weights();
}
