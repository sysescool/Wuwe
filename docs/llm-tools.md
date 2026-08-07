---
id: llm-tools
title: Typed tools
description: Define model-visible tools from C++ aggregates and compose providers.
---

# Typed tools

Wuwe derives a JSON tool schema from a C++ aggregate, parses model arguments into that type, invokes it, and serializes the result.

## Define a tool

```cpp
enum class temperature_unit { celsius, fahrenheit };

struct get_weather {
  static constexpr std::string_view description =
    "Get the current weather for a city.";

  std::string city;
  wuwe::field<temperature_unit> unit {
    .default_value = temperature_unit::celsius,
    .description = "Preferred temperature unit.",
  };

  std::string invoke() const {
    return city + " is 22 degrees.";
  }
};
```

Tool types are aggregates with a static or instance `description` and an `invoke()` method. Supported schema values include strings, booleans, numbers, enums, optionals, vectors, nested aggregates, and `field<T>` metadata.

`tool_field_traits<T, I>` can provide a field description or default when changing the aggregate member type is undesirable.

## Bind tools to a client

```cpp
auto runner = client->bind_tools<get_weather>();
const auto response = runner.complete("What's the weather in Tokyo?");
```

The type name becomes the default tool name. `make_llm_tool<T>()` exposes the generated schema, while `parse_tool_arguments<T>()` and `invoke_reflected_tool<T>()` provide lower-level control.

## Providers

```cpp
auto local = std::make_shared<wuwe::tool_provider<get_weather>>();
auto combined = wuwe::compose_tool_providers(local, another_provider);
```

`tool_provider<T...>` supplies schemas and dispatch for a set of types. `composite_tool_provider` and `compose_tool_providers()` combine providers while preserving each provider's implementation and state.

Context-aware tools can expose `invoke(context)`. Stateful modules such as memory, knowledge, and execution use dedicated provider classes built on the same model-facing tool contract.

## Tool Contract 2.0

The model-visible `llm_tool` remains a small name/description/input-schema value.
Framework and host policy use the richer `agent::tools::tool_descriptor`:

- version, input schema, and output schema;
- side-effect classification (`none`, `read`, `write`, `destructive`);
- idempotency semantics;
- approval mode;
- declared timeout;
- strict, warning, or disabled output-schema validation;
- bounded retry policy with backoff, jitter, and semantic error categories;
- optimistic resource-version preconditions and returned versions;
- explicit compensation and long-running heartbeat policies;
- capability requirements and host metadata;
- optional per-tool model-output projection ceilings.

Specialize `wuwe::tool_contract<T>` without changing the reflected tool type:

```cpp
template<>
struct wuwe::tool_contract<update_setting> {
  static wuwe::agent::tools::tool_descriptor descriptor() {
    auto descriptor = wuwe::agent::tools::descriptor_from_llm_tool(
      wuwe::make_llm_tool<update_setting>());
    descriptor.version = "2";
    descriptor.side_effect = wuwe::agent::tools::tool_side_effect::write;
    descriptor.idempotency =
      wuwe::agent::tools::tool_idempotency::idempotent;
    descriptor.approval = wuwe::agent::tools::tool_approval_mode::always;
    return descriptor;
  }
};
```

Providers that accept `tool_invocation` receive the descriptor, call ID,
idempotency key, execution context, deadline information, and stop token. Existing
`invoke(name, arguments)` and `invoke(name, arguments, stop_token)` providers remain
compatible. Advanced semantics are capability-based rather than inferred from an
adapter overload. A provider that actually consumes invocation fields declares
them explicitly:

```cpp
wuwe::agent::tools::tool_provider_capabilities contract_capabilities(
  std::string_view) const noexcept {
  return {
    .invocation_context = true,
    .idempotency_key = true,
    .heartbeat = true,
    .compensation = true,
  };
}
```

The reflected `tool_provider<T...>` adapter deliberately declares none of these
advanced capabilities because it invokes `T::invoke()` from model arguments and
does not pass the framework-generated idempotency key or heartbeat callback.
Wrapper providers must forward capabilities per tool. `composite_tool_provider`
does so and also routes compensation to the owning provider.

`llm_tool_result` is an alias of `tool_outcome`. It carries user/model-visible
text, structured JSON data, a standard error code, a semantic error category,
retryability, resource version, explicit partial-effect compensation state,
artifacts, and metadata. The runner sends failures to the model as
an explicit `{ "ok": false, "error": ... }` object rather than ambiguous success
text. The agent runner enforces a declared timeout as an execution deadline,
requests cooperative cancellation, returns without waiting for a non-cooperative
tool, and discards its late result. This bounds the agent loop but cannot roll back
an external side effect that the tool already started; effectful tools must honor
the stop token and idempotency key.

The standard runner validates input before dispatch and validates structured output
after the host's `prepare_tool_result` hook. Strict output validation converts a
schema violation into a structured tool failure; warning mode preserves the result
and records issues in metadata. The built-in validator covers the production JSON
Schema subset used by Wuwe tool contracts, including local `$ref`, types,
properties, required/additional properties, arrays, strings, numbers, enums, and
combinators. Known assertion keywords outside that subset fail closed instead of
being silently ignored.

Retries reuse the same call ID and idempotency key. More than one attempt is valid
only for idempotent tools or tools with compensation enabled. A non-idempotent
failure is retried only after the outcome explicitly sets `compensation_required`
and the provider's `compensate(invocation, failure)` succeeds. Wuwe does not infer
that a failed external mutation is safe to repeat.

Operational retries finish before `prepare_tool_result` and output validation.
Schema failures are not automatically retried, because repeating a tool after a
malformed success could duplicate an external effect. A host that can prove such a
retry safe should express that recovery inside its tool implementation.

## Model-visible output projection

Tool outcomes often need to remain complete for validation, audit, durable
idempotency, compensation, and application callbacks while being too large to send
back to the model. `llm_agent_runner` therefore applies a separate hard projection
boundary only when it constructs the model-facing Tool message.

```cpp
wuwe::llm_agent_run_options options;
options.tool_output_projection =
  wuwe::agent::llm::tool_output_projection_policy {
    .max_bytes = 64 * 1024,
    .max_tokens = 10'000,
  };
```

Both ceilings apply. Bytes are exact; tokens use the run's injected
`context_token_estimator`, or Wuwe's UTF-8-aware fallback estimator. The defaults
are 64 KiB and 10,000 estimated tokens. Policies below 256 bytes or 64 estimated
tokens are rejected. Before model or tool execution, the runner also validates
that the effective policy can contain every truncation envelope under the injected
estimator.

A tool contract can tighten, but never widen, the run-level policy:

```cpp
descriptor.model_output_projection = {
  .max_bytes = 8 * 1024,
  .max_tokens = 2'000,
};
```

Output below both limits is byte-for-byte compatible with the established Tool
message payload. Truncated plain text contains an explicit warning and a
UTF-8-safe head/tail preview. Truncated structured successes and failures remain
valid JSON; failures preserve their semantic category, standard error code, and
retryability. The complete `tool_outcome` still reaches `on_tool_result` and
durable admission. Only the projected Tool message is sent to the model and
recorded as the tool conversation message in Memory.

`on_tool_output_projection` observes every projection report.
`llm_agent_event_type::tool_output_truncated` is emitted only when content was
actually truncated. The report includes original and projected byte/token counts,
the effective ceilings, and the dimension that triggered projection.

For durable runs, every successful projection is linked to its admitted raw result
through `agent_run_record::tool_output_projections`. The audit stores the Tool call
identity, projected-content SHA-256, report, and timestamp, but not another copy of
the projected content. Replaying the same record is idempotent; rebinding a Tool
call ID to different projection data is rejected.

Standalone integrations can depend on the lightweight `text_token_estimator`
interface and call `check_tool_output_projection_policy()` during configuration.
`try_project_tool_output_for_model()` returns a typed
`tool_output_projection_result` for invalid policy, envelope, estimator, or JSON
serialization failures. The exception-based `project_tool_output_for_model()`
wrapper remains available for compatibility. The runner uses the typed-result API;
a result-specific failure after durable admission becomes
`llm_error_code::tool_output_projection_failed` and is never sent to the model.

Set `resource_version.require_expected_version` to require an optimistic version
at the configured argument JSON Pointer before invocation. A returned scalar at
the outcome pointer is exposed as `tool_outcome::resource_version`. Long-running
providers that accept `tool_invocation` can report progress through
`report_heartbeat`; legacy provider signatures remain compatible but cannot use
heartbeat, compensation callbacks, or `idempotent_with_key` retries. The runner
rejects those invalid contract/provider combinations before dispatch. Merely
adding an `invoke(tool_invocation)` forwarding overload does not grant advanced
capabilities.

## Security boundary

Schema validation is not authorization. A tool that reads files, calls services, changes state, or starts a process should enforce host policy at invocation time. The agent runner can reject calls through `allow_tool_call`; the Filesystem and Process modules add root or executable policy, approval, limits, and audit checks.

For local operations, prefer the structured [Filesystem toolkit](filesystem-tools.md)
and argv-first [Process toolkit](process-tools.md). Filesystem mutations do not
pass through a shell, and the raw shell adapter is disabled unless both policy
and provider configuration opt in.

See `examples/src/example.cpp` and `examples/src/simple_example.cpp` for built examples.
