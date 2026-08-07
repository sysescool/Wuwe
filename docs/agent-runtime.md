---
id: agent-runtime
title: Agent runtime
description: Run model and tool loops with durable state, approvals, cancellation, and host-owned policy.
---

# Agent runtime

`llm_agent_runner` coordinates model calls and typed tool calls. It remains host-application neutral: the application owns UI state, user identity, secrets, approvals, storage, and scheduling.

`agent_execution_context` carries run, trace, request, tenant, user, application,
workspace, conversation, agent, locale, deadline, cancellation, and host metadata
through the runner and tool invocation boundary. Durable fields serialize with a run;
the process-local `std::stop_token` is deliberately not serialized.

`execution_context_attributes()`, `make_approval_request()`,
`make_audit_event()`, `make_agent_event()`,
`memory_scope_from_execution_context()`, and
`knowledge_access_from_execution_context()` provide one authoritative projection
path instead of duplicating identity mapping in each module. Framework identifiers
override colliding attributes; host metadata is namespaced under
`context.metadata.*`.

Scope projection never combines two subjects. When an execution context supplies
any Memory identity field, that projected scope replaces the configured active
scope as a unit; a completely empty execution identity falls back to the active
scope for compatibility with the prompt-only runner API. Knowledge access follows
the same rule for tenant/user identity: an empty context preserves configured
access, while a supplied tenant or user replaces both subject fields so identities
from different requests cannot be accidentally mixed.

Execution context is identity and correlation state, not a credential container.
Metadata keys that represent tokens, passwords, credentials, API keys, or secrets
are omitted from default serialization and telemetry projection. Durable run
creation rejects them. Pass credentials through host-owned provider or tool
configuration instead.

## Basic run

```cpp
struct get_weather {
  static constexpr std::string_view description =
    "Get the current weather for a city.";

  std::string city;

  std::string invoke() const {
    return city + " is 22C.";
  }
};

auto client = wuwe::make_llm_client("OpenAI", config);
auto runner = client->bind_tools<get_weather>();
const auto response = runner.complete("What's the weather in Tokyo?");
```

The runner sends tool schemas to the model, parses tool-call arguments, invokes the matching provider, appends the result, and continues until a final response or the configured tool-round limit is reached.

## Observation and cancellation

```cpp
std::stop_source stop_source;

wuwe::llm_agent_run_options options;
options.stop_token = stop_source.get_token();
options.callbacks.on_delta = [](std::string_view delta) {
  // Render final-answer text as it arrives.
};
options.callbacks.on_tool_start = [](const wuwe::llm_tool_call& call) {
  // Record or display the tool transition.
};
options.callbacks.on_tool_result =
  [](const wuwe::llm_tool_call&, const wuwe::llm_tool_result&) {
    // Observe the completed tool call.
  };

const auto response = runner.complete("Inspect the input.", std::move(options));
```

The callback surface includes normalized stream events, content deltas, provider-supplied reasoning summaries, tool lifecycle events, completion, errors, and cancellation. `prepare_model_request`, `prepare_tool_call`, and `prepare_tool_result` provide explicit transformation or rejection points before a request or result advances through the loop; `on_model_result` observes every completed provider call without forcing streaming. Wuwe does not invent reasoning text when a provider does not supply it.

`on_tool_result` observes the complete validated tool outcome. The separate
`on_tool_output_projection` callback reports the bounded payload constructed for
the model, and `tool_output_truncated` is emitted on the native event stream only
when a hard byte or estimated-token ceiling was exceeded. Projection happens
after host preparation, output-schema validation, resource-version extraction,
and durable admission, so it cannot erase data required by those boundaries.
Global and per-tool projection policies are preflighted before tool dispatch. If
an estimator or serializer nevertheless fails on a specific admitted result, the
runner returns `llm_error_code::tool_output_projection_failed`, emits the normal
error callback, and does not issue another model request. Arbitrary estimator
exception text is not copied into model-visible content or durable response
metadata.

`llm_agent_run_options::persist_request_messages`, `persist_assistant_messages`, and `persist_tool_messages` default to `true`. Disable the relevant writes when a higher-level boundary must validate or select an execution before committing it to Memory. Reasoning does this automatically for isolated Best-of-N candidates, and Guarded Reasoning delays assistant persistence until the accepted or modified final output is available.

`run_async()` returns an `llm_agent_run` with `request_stop()`, `wait()`, and `get()`. Cancellation is cooperative across the runner, HTTP transport, and tools that honor the supplied stop token.
Destroying or replacing a live `llm_agent_run` requests cancellation and waits for
its accepted executor work to finish, preserving the lifetime guarantees of the
referenced client and provider. If destruction occurs from that run's own callback,
the handle requests cancellation and lets the independently owned task state unwind
after the callback returns instead of waiting for itself.

## Executors and scheduling

Async agent runs and tool invocations use bounded fixed-size executors rather than
creating one detached thread per operation. The default run and tool executors are
separate so a pool full of agent runs cannot deadlock while those runs wait for
tools. Hosts can inject their own services:

```cpp
auto runs = std::make_shared<wuwe::agent::runtime::thread_pool_executor>(
  wuwe::agent::runtime::thread_pool_options {
    .threads = 8,
    .queue_capacity = 256,
  });
auto tools = std::make_shared<wuwe::agent::runtime::thread_pool_executor>(
  wuwe::agent::runtime::thread_pool_options {
    .threads = 16,
    .queue_capacity = 512,
  });

wuwe::llm_agent_run_options options;
options.run_executor = runs;
options.tool_executor = tools;
options.scheduler = std::make_shared<
  wuwe::agent::runtime::timer_scheduler>();
```

`executor` and `scheduler` are small interfaces intended for dependency injection.
The scheduler owns retry backoff and offers cancellation-aware deadlines; tests can
provide deterministic implementations without sleeping. Custom implementations
create `scheduled_task_source`, return `source.task()`, and eventually call
`source.execute(work)`; this preserves cancellation, exception propagation,
self-wait detection, and completion bookkeeping without exposing internal state.

Accepted thread-pool work is drained during shutdown, while individual task
cancellation is delivered by `std::stop_token`. If the final executor owner is
released on one of its own workers, that worker completes the accepted queue from
shared state instead of joining itself. `timer_scheduler` dispatches due callbacks
through an executor, so a long callback does not block the timer thread. External
shutdown waits for already-dispatched callbacks; shutdown initiated from the
dispatch domain requests cancellation and lets that domain unwind without a
self-deadlock.

Run and tool executors must use independent execution domains because the run
blocks while waiting for its tool result. Wuwe rejects the same domain up front.
Custom executor adapters that share a bounded backend should return the same
`execution_domain()` value and implement `owns_current_thread()`.

Tool calls run behind a shared, bounded execution guard. A non-cooperative
tool may continue after the agent returns a timeout, but it retains an in-flight
slot until it exits. `max_in_flight_tool_invocations` defaults to 16 per
runner state and prevents repeated timeouts from creating unbounded background
work; capacity exhaustion is returned to the model as a retryable `unavailable`
tool outcome. Use an out-of-process execution boundary when a tool must be forcibly
terminated. Injected executors are retained by accepted work, so a temporary
`shared_ptr` does not turn an isolated timeout into an executor-destructor wait.

Long-running cooperative tools can call `tool_invocation::report_heartbeat` when
their descriptor declares a heartbeat timeout. Missing heartbeats request
cancellation and isolate the late result in the same way as a hard deadline.
`on_tool_heartbeat` receives rate-limited progress updates on the runner thread.

See [Context budget](context-budget.md) for unified request-window allocation.

## Skills integration

The independent [Skills module](skills.md) can resolve and activate reusable
instruction packages before a run. `apply_skill_activation()` preserves content
trust, labels Skill messages for separate context budgeting, and exposes only the
Tools declared by the activation. Wrap the actual provider with
`scoped_tool_provider` as well; filtering model-visible schemas alone is not an
invocation security boundary. Skills never grant capabilities or skip the normal
Tool Contract, approval, cancellation, and audit path.

## Durable runs

`agent_run_runtime` adds a storage-neutral state machine:

```text
created -> running -> waiting_for_approval -> running
                   -> completed | failed | cancelled | timed_out
```

Use `in_memory_agent_run_store` for tests and `sqlite_agent_run_store` for local
durability. The SQLite store uses WAL, transactions, a schema version, and a busy
timeout. Store writes require an expected revision. Every applied mutation advances
the revision and appends one event with the same monotonic sequence, so a host can
resume event delivery with `list_events(run_id, after_sequence)`.

Durable Tool handling keeps the admitted raw `tool_outcome` separate from the
model-facing projection audit. `tool_output_projections` stores one idempotent
record per Tool call containing the projected-content SHA-256, projection report,
effective byte/token ceilings, and timestamp. It does not duplicate the projected
body. The `tool_output_projection_recorded` event exposes the same digest and
counts, allowing hosts to link raw execution evidence to the bounded representation
without creating another sensitive-content copy.

```cpp
namespace runtime = wuwe::agent::runtime;

auto store = std::make_shared<runtime::sqlite_agent_run_store>("agent-runs.db");
auto durable = std::make_shared<runtime::agent_run_runtime>(store);

wuwe::llm_agent_run_options options;
options.context = {
  .tenant_id = "tenant-1",
  .user_id = "user-1",
  .application_id = "product",
  .conversation_id = "conversation-1",
};
options.runtime = durable;

const auto response = runner.complete("Apply the approved change.", options);
```

`cancel(run_id, expected_revision, reason)` provides an explicit optimistic
cancellation operation. A `conflict` result reports the current revision without
overwriting concurrent state.

Set `llm_agent_run_options::pricing` to calculate `llm_response::cost` from the
usage accumulated across every model call in the tool loop. Terminal durable
results persist the five usage counters and the full cost breakdown. Approval
continuations also persist accumulated usage and pricing, so a resumed process does
not lose accounting state when the host omits pricing from the resume options.
They also persist the resolved Tool-output projection policy. A resume override
may tighten either ceiling but cannot widen the policy that governed the original
run.

## Cross-request approval continuation

Tools whose descriptor requires approval are authorized before any tool in the
same model-proposed batch executes. A manual-review decision moves the run to
`waiting_for_approval` and returns `run_id`, `revision`, `approval_id`,
`tool_call_id`, and a continuation token in response metadata.

```cpp
auto waiting = durable->get(run_id);
auto approved = durable->resolve_approval(
  run_id,
  waiting->revision,
  continuation_token,
  runtime::approval_resolution::approved,
  "approved by operator");

auto completed = runner.resume(
  run_id, approved.revision, continuation_token, options);
```

The continuation persists the complete model request, exact pending calls, used
tool-round count, and approvals already granted within a multi-call batch. The
token is a bearer secret: return it only to an authorized host flow and do not put
it in ordinary logs or model-visible content. Approval callbacks cannot weaken an
`always`/`policy` requirement declared by the tool contract.

This token-bearing API is the trusted in-process Runtime contract. The public
[Agent Host protocol](agent-host-protocol.md) never serializes the token: Host
clients use `approvalId`, and the authorized Service resolves that identifier to
the internal token.

After a worker claims an approved continuation, the store retains it as active
continuation state until the run reaches a terminal state or suspends again. A host
can therefore reclaim it after a process failure using the latest revision. A
reclaim invalidates the previous worker's revision; effectful tools must still use
the supplied idempotency key because a remote side effect can race with takeover.

Tool outcomes are admitted to the run before the loop advances. Re-admitting the
same call ID and idempotency key returns the original outcome without a second
event, including when the retry carries an old revision. On recovery, the runner
reuses admitted outcomes. Durable admission stores the complete outcome; the
model-visible projection is derived afterward and is not substituted into the
audit/idempotency record. This prevents duplicate result submission; it does not
make an arbitrary external side effect exactly-once across a crash between the
effect and admission. Effectful providers should honor
`tool_invocation::idempotency_key` or implement their own transactional boundary.

SQLite run stores maintain an explicit component schema version and apply ordered migrations inside an immediate transaction. Unversioned legacy run tables migrate to the current schema; databases created by a newer Wuwe version are rejected rather than opened with unknown semantics. `schema_version()` exposes the active version for readiness checks.

All Store families expose the shared capability vocabulary described in
[Storage contracts](storage-contracts.md). `agent_run_runtime::store_capabilities()`
allows readiness checks without downcasting to a concrete adapter.

`replay_run_events()` converts durable `agent_run_event` records to the common observability envelope and publishes events after an exclusive sequence cursor. This is suitable for SSE reconnects, audit reconstruction, and trace regression without inventing a second event format.

Use the [Agent Host protocol](agent-host-protocol.md) when a service or sidecar
needs a stable cross-language boundary for these run operations.

## Memory

Constructors accept an optional `memory_context`. When present, the runner augments requests from scoped memory and records new conversation state through that context. The application still controls the memory policy and backing store.

## Policy boundary

`llm_agent_callbacks::allow_tool_call` can reject a proposed tool call before dispatch. Prepared tool calls are used both for invocation and for the assistant tool-call message sent to later model rounds. Security-sensitive tools should also enforce their own capability, approval, path, and audit policies; a model-facing tool schema is not an authorization boundary.

Use [Reasoning](reasoning.md) when the run needs explicit modes, budgets, or traces. Use [Planning](planning.md) for dependency-aware multi-step execution.
