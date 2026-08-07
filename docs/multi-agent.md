---
id: multi-agent
title: Multi-agent runtime
description: Register specialized agents, share team sessions, delegate tasks, run parallel work, and resolve consensus.
---

# Multi-agent runtime

The Multi-Agent module provides a local team runtime above individual model and tool loops. It owns agent registration, skill-based dispatch, lifecycle and capacity checks, shared sessions, task state, parallel execution, consensus, cancellation, telemetry, and Planning integration.

`multi_agent::agent_skill` is a lightweight routing summary, not an executable
package. The independent [Skills module](skills.md) owns strict manifests,
resources, dependencies, and activation. Use
`<wuwe/agent/multi_agent/skills_adapter.hpp>` to publish a verified package as
routing metadata; the projection deliberately does not transfer package ownership
or runtime authorization.

## Agent contract

An `agent_descriptor` declares stable identity, role, skills, metadata, and `max_concurrency`. An `agent_executor` performs one `agent_task_request` with a shared `team_session`, `stop_token`, effective `deadline`, and `remaining_time()` helper:

```cpp
namespace ma = wuwe::agent::multi_agent;

auto registry = std::make_shared<ma::agent_registry>();
registry->add({
  .id = "reviewer",
  .name = "Reviewer",
  .role = "quality-review",
  .skills = { {
    .id = "review",
    .name = "Review text",
  } },
  .max_concurrency = 2,
}, std::make_shared<ma::function_agent_executor>(
  [](const ma::agent_task_request& request,
     const ma::agent_execution_context& context) {
    if (context.cancellation_requested()) {
      return ma::agent_task_result {
        .status = ma::agent_task_status::cancelled,
        .error_code = ma::agent_task_error_code::cancelled,
      };
    }
    return ma::agent_task_result { .output = "reviewed: " + request.input };
  }));

ma::team_runtime team({ .registry = registry });
const auto result = team.run({
  .input = "Draft text",
  .required_skills = { "review" },
});
```

Dispatch applies hard constraints before deterministic load selection. A preferred agent must exist, satisfy every required skill, be `available`, and have free capacity. Non-concurrent executors cannot advertise a concurrency greater than one. `draining` and `offline` agents receive no new work; active leases remain valid until their task finishes.

`agent_executor::execute()` is a synchronous boundary. It must return a stable paused or terminal state: `input_required`, `completed`, `failed`, `blocked`, `cancelled`, or `timed_out`. Returning `submitted` or `working` is treated as an executor contract failure because the runtime cannot safely release the capacity lease while claiming that work is still active. Use `team_runtime::run_async()` to make the caller-facing operation asynchronous.

## Shared sessions

`team_session` is a thread-safe collaboration boundary. It stores user and agent messages, artifacts, JSON shared state, task status, and host metadata. Parallel agents receive the same session object, while `snapshot()` returns an immutable copy suitable for aggregation and reporting. Task admission is atomic: the runtime rejects an ID that is already submitted, working, completed, or timed out. Failed, blocked, cancelled, and input-required tasks may be retried or continued with the same logical ID.

The runtime records request input, successful or failed task state, agent output, and returned artifacts. Application-owned shared state remains explicit through `set`, `get`, and `erase`.

## Parallel work and consensus

`max_parallel_tasks` is a runtime-wide concurrency limit shared by direct, asynchronous, parallel, and consensus callers. `run_parallel()` uses at most that many coordinator threads, preserves input order, and passes every execution through the same shared limiter. A coordinator thread creation failure falls back to caller-thread coordination rather than abandoning already admitted work. Agent-level `max_concurrency` is still enforced independently by the registry.

`reach_consensus()` sends one task to explicitly named participants or every available skill-compatible agent. `minimum_successful_agents` controls quorum; `minimum_agreement` controls matching votes. When `minimum_agreement` is zero, strict majority among successful results is required.

The default resolver compares exact output strings. Semantic voting, weighted roles, judge agents, rubric scoring, or domain-specific merge behavior should be supplied through `consensus_resolver`; Wuwe does not pretend that free-form outputs are equivalent without an explicit policy.

The consensus request is itself recorded as a session task. A successful resolver commits its final message and artifacts after participant results; resolver exceptions and invalid artifacts become structured consensus failures. Cancellation before dispatch is preserved as `cancelled`, rather than being reported as failed agreement.

## Planning integration

`team_plan_executor` implements the existing `planning::plan_executor` contract. A step with `assigned_agent` becomes a team task whose session is the plan ID. Executor cancellation and concurrency capabilities are derived from the registered agent.

Use `composite_plan_executor` when one plan contains both tool-assigned and agent-assigned steps.

## Operations boundary

Typed `team_observer` events and the common `observability::event_sink` expose task and consensus lifecycle without carrying request input or successful output. Failure messages remain available for diagnosis. Telemetry exceptions are isolated by default and can be configured to propagate.

`agent_task_request::timeout` overrides `default_task_timeout`. A timeout covers capacity waiting and execution. If an executor ignores cancellation, the call returns promptly with `timed_out` and `detached = true`, while the runtime keeps the executor, lease, session, and concurrency slot alive until the worker exits.

Task admission is guarded until a stable terminal or paused status is committed. If strict telemetry or another exception escapes after admission, the guard changes a still-submitted or still-working task to `failed`, allowing a deliberate retry instead of permanently poisoning the task ID.

Cancellation is cooperative. A remote process, thread, tool, or model call must honor its token or provide a stronger host-owned termination boundary. The runtime is an SDK component; it does not create identities, permissions, durable queues, or distributed locks.
