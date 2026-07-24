---
id: evaluation
title: Evaluation
description: Run reusable output, structured-result, and trajectory evaluators with stable suite metrics.
---

# Evaluation

The Evaluation module provides a common contract for offline regression suites, CI gates, and sampled production evaluation. It does not prescribe a model judge or storage platform.

An `evaluation_case` can contain input, output, expected output, structured actual and expected values, a JSON trajectory, elapsed time, and metadata. Each `evaluator` returns a normalized score, pass decision, explanation, evidence, and metadata.

```cpp
namespace eval = wuwe::agent::evaluation;

eval::evaluation_runner runner({
  .policy = { .pass_threshold = 0.8 },
});

runner.add(std::make_shared<eval::exact_match_evaluator>(), 2.0);
runner.add(std::make_shared<eval::contains_terms_evaluator>(
  eval::contains_terms_options {
    .terms = { "citation", "source" },
  }));

const auto suite = runner.run(cases);
```

For end-to-end evaluation, pass an `evaluation_target` as the second argument to `run()`. The target executes each seed case and returns its actual output, structured data, and trajectory. Target exceptions are isolated as failed cases instead of aborting the suite.

Weights control score aggregation. Evaluator names are validated and frozen when they are registered; empty or duplicate names are rejected, and the finite aggregate weight must remain representable. Required evaluators can fail a case independently of its aggregate score. Thresholds, weights, and evaluator scores must be finite; thresholds must be in `[0, 1]`, while returned scores are clamped to that range after validation. With `fail_fast = true`, evaluation stops after the first failed required check only when `require_all_required_checks` is enabled. When required checks are not mandatory, every evaluator still runs so the aggregate threshold is computed from the complete configured set. Evaluator exceptions fail the case by default and remain isolated as check diagnostics; `evaluator_failure_mode::ignore` must be selected explicitly to exclude unavailable evaluators.

## Trajectory regression

`trajectory_evaluator` checks required events, ordered event subsequences, forbidden events, and maximum event counts. Include `<wuwe/agent/evaluation/reasoning_evaluation.hpp>` for the optional Reasoning adapter. `evaluation_case_from_reasoning()` converts a `reasoning_result` into an evaluation case with structured result data and normalized reasoning trace events. `evaluation_case_from_best_of_n()` does the same for candidate generation, scoring, rejection, and selection trajectories.

```cpp
runner.add(std::make_shared<eval::trajectory_evaluator>(
  eval::trajectory_expectation {
    .required_events = { "model_started", "tool_completed" },
    .required_sequence = { "model_started", "tool_started", "tool_completed" },
    .forbidden_events = { "failed" },
    .maximum_occurrences = { { "tool_started", 3 } },
  }));
```

Use `function_evaluator` to adapt LLM judges, grounding checks, policy compliance checks, task-specific assertions, or external evaluation services. `evaluation_suite_result_to_json()` provides a stable report envelope for CI artifacts and downstream telemetry.

`make_candidate_evaluator_scorer()` also adapts any common `evaluator` into a Best-of-N scorer. Its metric score becomes the candidate score, and its pass decision can act as an eligibility gate. This keeps online candidate selection and offline regression checks on the same evaluation contract.

Observer and common event-sink failures are ignored and reported in case metadata by default. Configure `telemetry_failure_mode = propagate` when strict telemetry delivery is part of the evaluation gate.

Knowledge retrieval keeps its domain-specific `knowledge_eval` metrics such as recall-at-k and mean reciprocal rank. Reflection remains an online candidate-review mechanism. Both can coexist with the common Evaluation module rather than being redefined by it.
