---
id: reasoning
title: Reasoning
description: Run explicit reasoning modes and Best-of-N selection with budgets, traces, tools, reflection, and plans.
---

# Reasoning

`reasoning_runner` selects and executes a bounded strategy around model calls, tools, reflection, or planning. It reports a normalized result, resource usage, and a structured trace.

## Modes

| Mode | Behavior |
| --- | --- |
| `simple` | One model-oriented answer path without a tool loop |
| `react` | Model and tool loop through `llm_agent_runner` |
| `reflect_and_retry` | Generate, evaluate, and revise or retry within policy limits |
| `plan_execute` | Create and execute a dependency-aware plan |

## Best-of-N reasoning

`best_of_n_runner` generates a bounded set of independent candidates, scores every completed candidate, and applies a deterministic selector. It is a coordination layer around `reasoning_runner`, rather than another single-run mode: each candidate may independently use simple, ReAct, reflection, or planning reasoning.

```cpp
namespace reasoning = wuwe::agent::reasoning;

reasoning::best_of_n_runner best_of_n({
  .generator = reasoning::make_reasoning_candidate_generator(base_runner),
  .scorer = [](const reasoning::reasoning_request& request,
               const reasoning::reasoning_result& candidate,
               const reasoning::best_of_n_context& context) {
    return reasoning::best_of_n_score {
      .value = score_answer(request.input, candidate.content),
      .accepted = passes_required_checks(candidate),
      .rationale = "task-specific quality score",
    };
  },
  .request_builder = [](const reasoning::reasoning_request& base, std::size_t index) {
    auto request = base;
    request.temperature = 0.2 + 0.15 * index;
    return request;
  },
});

const auto result = best_of_n.run(request, {
  .policy = {
    .candidate_count = 4,
    .max_concurrency = 2,
    .timeout = std::chrono::seconds(30),
    .minimum_score = 0.7,
    .budget = {
      .max_model_calls = 4,
      .max_total_tokens = 12000,
      .estimated_model_calls_per_candidate = 1,
      .estimated_total_tokens_per_candidate = 3000,
      // Add scorer estimates here when scoring calls a judge model:
      .estimated_scorer_model_calls_per_candidate = 0,
      .estimated_scorer_total_tokens_per_candidate = 0,
    },
  },
  .stop_token = stop_token,
});

if (const auto* selected = result.selected_candidate()) {
  use(selected->result.content);
}
```

Candidate order is stable even when completion order differs. Generation, scoring, rejection, aggregate-budget exhaustion, side-effect rejection, cancellation, timeout, skipped work, and detached uncooperative work remain distinct in `best_of_n_candidate_status`. `aggregate_usage` contains usage reported by returned candidate results. `budget_accounted_usage` additionally includes scorer/generator accounting and reservations still owned by detached work; `outstanding_reserved_usage` exposes that unresolved estimate separately. A scorer that calls a judge model should return that call's `reasoning_usage` in `best_of_n_score::usage`, so it participates in the same accounting.

`best_of_n_budget` is shared by the complete candidate set. Generation and scoring have separate per-candidate model-call, total-token, and USD estimates. Each stage is reserved before its callback starts and reconciled with reported usage afterward; set the `estimated_scorer_*` fields whenever scoring calls a judge model. When a global limit is configured, the combined generation-plus-scoring estimate must be non-zero and the requested candidate set must fit during preflight. This prevents a nominal per-candidate budget from silently expanding to `N` times the intended run budget, and prevents concurrent scorers from bypassing the remaining capacity.

The default selector chooses the highest score. Scores within `score_tie_tolerance` are resolved by lower accounted cost, fewer tokens, and then candidate index. Supply `best_of_n_selector` for domain-specific selection, or use `make_majority_vote_selector()` for self-consistency voting over an application-defined normalized answer key. A selector must return the index of an eligible candidate.

`make_reasoning_candidate_generator()` adapts an existing `reasoning_runner`. Candidate execution is isolated by default: tool calls and plan execution are denied, and request, assistant, tool, plan, memory, and reflection records are not persisted. After selection, call `commit_best_of_n_result(base_runner, result)` to persist only the selected final response. Set `best_of_n_options::side_effects` to `allow` only when repeated tool or plan side effects are intentional and independently idempotent. Isolation is enforceable for the built-in adapter; custom generator, scorer, builder, and selector callbacks remain application code and must honor `best_of_n_context::side_effects` themselves. Reported tool or plan usage is rejected in isolation mode, but the framework cannot roll back an undisclosed external mutation already performed by a custom callback.

When candidates run concurrently, the model client, scorer, evaluator, observers, guardrails, router, and other shared services must support concurrent calls; otherwise set `max_concurrency = 1` or provide isolated services. The built-in observer delivery is serialized, but generator and scorer callbacks are concurrent. Observer failures are ignored and counted by default; set `telemetry_failure_mode = propagate` for strict diagnostic environments.

The telemetry failure count is settled after the terminal event on every success, failure, timeout, and cancellation path. Worker-originated observer callbacks are stopped and any callback already in flight is allowed to leave the serialized observer boundary before `run()` returns.

The generator and scorer receive a cancellation token and effective deadline. Use `contextual_request_builder` and `contextual_selector` when those callbacks also need direct access to the context; legacy builders and selectors are still executed inside the same bounded operation. `run_async()` returns a movable `best_of_n_run` handle with `request_stop()`, `wait()`, and `get()`.

As with general fan-out, an arbitrary callback cannot be forcibly terminated. A callback that ignores cancellation is reported as detached. Wuwe retains the callback object, candidate request, candidate snapshot, and dependencies captured by value until it exits. It cannot extend the lifetime of objects captured by raw pointer or reference; use value ownership or `std::shared_ptr` for anything that may outlive `run()`.

## Tool-using run

```cpp
namespace reasoning = wuwe::agent::reasoning;

auto provider = std::make_shared<wuwe::tool_provider<get_weather>>();
auto runner = reasoning::reasoning_runner::with_tools(
  *client,
  provider,
  {
    .observer = [](const reasoning::reasoning_event& event) {
      if (event.type == reasoning::reasoning_event_type::content_delta) {
        std::cout << event.delta << std::flush;
      }
    },
  });

const auto result = runner.run({
  .input = "What's the weather in Tokyo?",
  .model = config.model,
  .policy = {
    .mode = reasoning::reasoning_mode::react,
    .budget = { .max_tool_rounds = 2 },
    .enable_streaming = true,
  },
});
```

## Budgets and results

`reasoning_budget` limits steps, model calls, tool calls, tool rounds, reflection attempts, elapsed time, prompt tokens, completion tokens, total tokens, and accounted USD cost. Exceeding a budget produces a typed reasoning error instead of silently continuing. `estimated_output_tokens_per_call` controls token and cost preflight; provider usage replaces estimates when available.

Token and cost accumulation is saturating and treats overflow, negative cost, and non-finite cost as budget failures. Provider totals smaller than their prompt-plus-completion components are conservatively raised to the component total, preventing malformed usage reports from reopening exhausted capacity.

`reasoning_result` contains:

- completion state, content, and provider-supplied reasoning summary;
- final model response;
- optional plan and reflection runs;
- steps, trace records, model-routing decisions, token/cost accounting, and usage counts;
- typed and underlying errors;
- elapsed time.

The observer receives lifecycle, stream, tool, reflection, planning, completion, failure, and cancellation events. `run_async()` adds cooperative cancellation.

Attach `reasoning_runner_options::model_router` to select a model before every model call. The router receives capability requirements, estimated tokens, and the remaining cost budget, allowing later Tool Loop rounds or Reflection retries to move to a cheaper model. See [Resource-aware routing](resource-routing.md) for profiles, strategies, pricing, and execution boundaries.

## Policy selection

`select_policy()` maps a `reasoning_task_profile` to a built-in policy. The available profiles cover simple answers, tool-required work, complex analysis, required planning, and high-confidence answers. Hosts can bypass selection and populate `reasoning_policy` directly.

`make_default_agentic_runner()` assembles the standard model/tool/reflection path. `make_knowledge_aware_runner()` adds the knowledge tool provider. These helpers compose existing public modules; they do not hide capability or storage policy.

See `examples/src/reasoning_example.cpp` for offline plan execution and a live ReAct run. `best_of_n_reasoning_example.cpp` demonstrates offline candidate generation, scoring, selection, and aggregate usage.
