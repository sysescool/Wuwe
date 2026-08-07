---
id: orchestration
title: Orchestration
description: Compose synchronous, strongly typed agent flows with branching, bounded fan-out, fan-in, retry, cancellation, and recovery.
---

# Orchestration

The orchestration module composes model calls and ordinary C++ transformations into an in-process, strongly typed flow. Each step remains visible in code, and the final flow runs synchronously through `invoke()`. Independent branches can run concurrently through the same typed composition layer without introducing a Planning graph or Agent team.

## Build a flow

Include `<wuwe/agent/orchestration/flow_primitives.hpp>` for the flow primitives and `<wuwe/net/net_errc.h>` for the retry predicate below.

```cpp
auto to_prompt = [](const std::string& input) {
  return "Extract the technical details from:\n\n" + input;
};

auto pipeline =
  client
  | to_prompt
  | wuwe::tee([](const wuwe::llm_response& response) {
      wuwe::println("first pass: {}", response.content);
    })
  | wuwe::filter(
      [](const wuwe::llm_response& response) {
        return response && !response.content.empty();
      },
      "model returned no content")
  | wuwe::retry_if(
      [](const wuwe::llm_response& response) {
        return response.error_code == wuwe::net_errc::rate_limited;
      },
      1)
  | wuwe::recover([](const std::exception& error) {
      return wuwe::llm_response {
        .content = std::string("flow failed: ") + error.what(),
      };
    });

const auto response = pipeline.invoke(input);
```

When a step returns a string, string view, C string, or `llm_request`, the flow sends it to the bound `llm_client`. Other values continue to the next step unchanged. This allows prompt construction, model calls, validation, routing, and result conversion to share one typed pipeline.

`invoke(input, flow_context)` and `invoke(input, stop_token)` propagate cancellation to context-aware steps and implicit LLM calls. A step may accept `(value)`, `(value, stop_token)`, or `(value, const flow_context&)`.

## Fan-out and fan-in

Include `<wuwe/agent/orchestration/orchestration.hpp>` for the complete public orchestration API.

`fan_out` broadcasts one input to a fixed set of typed branches. Every branch must return the same value type; `void` is normalized to `std::monostate`. Results always follow branch declaration order, regardless of completion order:

```cpp
auto candidates = wuwe::fan_out(
  wuwe::fan_out_options {
    .max_concurrency = 2,
    .failure_mode = wuwe::fan_out_failure_mode::collect_all,
    .timeout = std::chrono::seconds(10),
  },
  [](const request& value, const wuwe::fan_out_context& context) {
    return run_candidate_a(value, context.stop_token);
  },
  [](const request& value, std::stop_token stop_token) {
    return run_candidate_b(value, stop_token);
  });

auto result = candidates.run(value, stop_token);
```

`fan_out_each` applies one worker to a runtime-sized, random-access range. It is the natural primitive for dynamic prompt candidates, batch retrieval, parallel tool calls, or later Best-of-N reasoning:

```cpp
auto parallel_map = wuwe::fan_out_each(
  wuwe::fan_out_options { .max_concurrency = 4 },
  [client](const llm_request& request, const wuwe::fan_out_context& context) {
    return client->complete(request, context.stop_token);
  });

auto responses = parallel_map.run(requests, stop_token);
```

Both variants return `fan_out_result<T>`. Each indexed item contains its status, optional value, exception, stable error text, elapsed time, and whether unfinished work was detached. Aggregate counters distinguish completed, failed, cancelled, timed-out, skipped, and detached branches.

The decayed input object remains alive for detached work and is exposed to branches as a `const` reference. Callers remain responsible for storage referenced by non-owning inputs such as `std::span` and `std::string_view`. Result values may be move-only but must be move-constructible. `fan_out_each` invokes the same const worker concurrently, so any state captured by that worker must be safe for concurrent access.

Use `fan_in_all()` when every branch is required, `fan_in_successes()` when dropping failures is an explicit policy, or `fan_in(reducer)` for voting, ranking, merging, quorum, or domain-specific aggregation. All three are ordinary Flow steps:

```cpp
auto pipeline = client
  | prepare
  | wuwe::fan_out(branch_a, branch_b, branch_c)
  | wuwe::fan_in([](wuwe::fan_out_result<candidate> result) {
      return select_best(std::move(result));
    });
```

`collect_all` schedules every branch and preserves partial failures. `fail_fast` stops scheduling after the first observed failure and requests cancellation from active siblings. Operation timeout and external cancellation also stop new scheduling immediately.

`fan_out_options::timeout` is relative to the start of that fan-out operation. `fan_out_options::deadline` is an absolute `std::chrono::steady_clock` deadline and is useful when several phases share one end-to-end budget. When both are present, the earlier boundary wins. An already-expired deadline returns a timed-out result without starting new branch work.

C++ cannot terminate an arbitrary function safely. If an active branch ignores its token when timeout, cancellation, or fail-fast occurs, the returned item is marked `detached`; the framework keeps the branch callable, input, and execution state alive until that work actually exits. Hosts must reconcile possible side effects before retrying detached work.

## Flow primitives

| Primitive | Responsibility |
| --- | --- |
| `identity` | Pass a value through unchanged |
| `tee` | Run a side effect such as logging while preserving the value |
| `apply_if` | Apply a transformation only when a predicate matches |
| `filter` | Reject a value by throwing `filter_error` |
| `if_else` | Select one of two transformations |
| `when`, `otherwise`, `route` | Route a value through the first matching branch |
| `fan_out` | Broadcast one value to fixed parallel branches with bounded concurrency |
| `fan_out_each` | Parallel-map a runtime-sized random-access range |
| `fan_in`, `fan_in_all`, `fan_in_successes` | Aggregate structured branch results under an explicit policy |
| `retry_if` | Re-run the composed upstream flow when its result matches a predicate |
| `recover` | Convert an exception from the upstream flow into a fallback value |

Branches must produce compatible return types because the flow is checked by the C++ type system. `retry_if` also requires the original input to be copy constructible.

## Boundary

Orchestration is a lightweight composition layer, not a persistent workflow engine, task queue, or distributed scheduler. Use [Planning](planning.md) when work needs dependency graphs, checkpoints, replanning, approvals, or resumable step execution.

See `examples/src/chain_example.cpp`, `flow_example.cpp`, `fan_out_example.cpp`, and `routing_example.cpp` for built examples.
