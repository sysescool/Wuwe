---
id: planning
title: Planning
description: Build, validate, execute, persist, and resume dependency-aware plans.
---

# Planning

The planning module turns a goal into a validated graph of steps and executes ready steps under explicit retry, timeout, approval, and persistence policies.

## Planners and executors

| Component | Implementations |
| --- | --- |
| Planner | `static_planner`, `llm_planner` |
| Executor | `function_plan_executor`, `tool_plan_executor`, `agent_plan_executor`, `composite_plan_executor` |
| Store | `in_memory_plan_store`, `file_plan_store` |

`llm_planner` requests a JSON plan, repairs normalizable output, and validates tool and agent assignments. `static_planner` is useful for deterministic workflows and tests.

For registered teams, `multi_agent::team_plan_executor` resolves `assigned_agent` through the Multi-Agent registry instead of a callback map. It preserves team lifecycle, capacity, shared-session, cancellation, artifact, and telemetry semantics. See [Multi-agent runtime](multi-agent.md).

Planning/Multi-Agent integration is an optional adapter. Include `<wuwe/agent/multi_agent/planning_adapter.hpp>` explicitly when using it; the core Multi-Agent umbrella does not pull Planning into every translation unit.

## Minimal plan

```cpp
namespace planning = wuwe::agent::planning;

auto planner = std::make_shared<planning::static_planner>(
  std::vector<planning::plan_step> {
    { .id = "inspect", .title = "Inspect input" },
    {
      .id = "summarize",
      .title = "Summarize result",
      .depends_on = { "inspect" },
    },
  });

auto executor = std::make_shared<planning::function_plan_executor>(
  [](const planning::plan_step& step,
     const planning::plan_execution_context&) {
    return planning::plan_step_result::completed(step.id + " completed");
  });

planning::plan_runner runner({
  .planner = planner,
  .executor = executor,
});

const auto result = runner.run({ .goal = "Inspect and summarize the input" });
```

## Execution semantics

`plan_runner` validates the graph, finds dependency-ready steps, executes up to `max_parallel_steps`, records outputs and artifacts, and persists checkpoints when a store is configured.

`plan_policy` controls maximum steps and iterations, attempts per step, steps per run, parallelism, step and run timeouts, replanning, failure continuation, and resume behavior. `resume()` resets interrupted running steps by default and continues from persisted state.

Runner construction rejects zero iteration or attempt limits, negative timeouts, non-positive cancellation polling, and invalid prioritization weights. Executor callback wrappers reject missing providers, empty names, and duplicate agent registrations at configuration time, so these errors cannot surface halfway through a plan.

Parallel steps receive one immutable snapshot of the plan and artifacts for their execution batch. The runner applies results to the live plan only on its scheduling thread, so executors never observe concurrent mutation of `plan_execution_context::current_plan`.

## Deadlines and cancellation

Step and run timeouts are scheduling deadlines, not post-execution duration checks. Each running step receives:

- a step-local `stop_token`;
- the earliest applicable step or run `deadline`;
- `cancellation_requested()`, `deadline_reached()`, and `remaining_time()` helpers.

An executor advertises its behavior through `plan_executor_capabilities`:

```cpp
auto executor = std::make_shared<planning::function_plan_executor>(
  [](const planning::plan_step& step,
     const planning::plan_execution_context& context) {
    while (!context.cancellation_requested()) {
      // Perform one bounded unit of work.
    }
    return planning::plan_step_result::failed("cancelled");
  },
  planning::plan_executor_capabilities {
    .cooperative_cancellation = true,
    .concurrent_execution = true,
  });
```

`cooperative_cancellation` declares that the executor is expected to observe the token. `concurrent_execution = false` makes the runner serialize calls to that executor even when the plan permits greater parallelism.

When a deadline or external stop request occurs, the runner requests cancellation and returns without waiting indefinitely. C++ cannot safely terminate an arbitrary thread. If an executor does not finish when cancellation is requested, the framework keeps the copied step, immutable plan snapshot, executor, and task state alive independently of the returned plan result. Executor callbacks must likewise own any resources they use after returning control to the caller. The step metadata contains:

- `stop_reason`;
- `cancellation_requested`;
- `executor_cooperative_cancellation`;
- `execution_detached`.

`plan_run_result::steps_timed_out` and `steps_detached` expose aggregate counts. A run deadline reports `plan_run_stop_reason::run_timeout` and emits `plan_event_type::plan_timed_out`.

Deadline settlement uses the completion timestamp recorded by the worker, not merely the time at which the scheduler observes a ready future. A result completed before its deadline remains valid even if scheduler wake-up is slightly late; a result completed after the deadline is not admitted as a successful step.

Detached execution may still produce external side effects. The runner therefore does not automatically retry or replan a detached timed-out step. On resume, detached running steps are also preserved by default. A host must reconcile the external operation before explicitly setting `reset_detached_steps_on_resume = true` or resetting the step itself. Strong termination of untrusted or non-cooperative work requires a process or sandbox backend that can terminate the underlying process.

Policy hooks can allow, deny, or require approval for each ready step. An approval provider supplies the host decision. A reflection gate can review completed steps before the run proceeds.

## Prioritization

Dependency readiness remains a hard constraint. Within the current ready set, `plan_prioritizer` dynamically orders steps from the step's `priority`, `urgency`, `expected_value`, `estimated_cost`, and optional wall-clock `deadline`. `plan_prioritization_policy` controls the weights and deadline horizon; equal scores use the earlier deadline and then original plan order.

Applications can provide `plan_runner_options::priority_scorer` to replace the default score while retaining validation and deterministic ordering. Returning a non-finite score is rejected. Setting `prioritization.enabled = false` preserves the original ready-step order.

## Validation and persistence

`plan_validator` checks identifiers, dependencies, cycles, limits, tool assignments, and agent assignments. `plan_normalizer` repairs safe structural issues. `plan_codec` serializes plans and results as JSON.

The file store is suitable for local checkpoints. Applications that need transactional shared storage can provide a custom `plan_store` backed by their database.

Planning events and trace callbacks expose plan creation, step transitions, approval requirements, revisions, cancellation, and completion.
