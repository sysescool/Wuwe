---
id: guardrails
title: Guardrails
description: Compose input, output, tool, planning, retrieval, and memory safety checks behind one runtime contract.
---

# Guardrails

Guardrails evaluate data at an agent boundary and return one of four explicit decisions: `allow`, `modify`, `deny`, or `require_approval`. They complement capability policy: capability policy decides whether an operation is authorized, while guardrails inspect the content or structured data involved in that operation.

## Pipeline

`guardrail_pipeline` executes ordered `guardrail` implementations. A modification becomes the input to subsequent checks. Denial and approval decisions stop the pipeline. Exceptions fail closed by default; applications must explicitly select `guardrail_failure_mode::open` when availability is more important than enforcement.

```cpp
namespace gr = wuwe::agent::guardrails;

auto pipeline = std::make_shared<gr::guardrail_pipeline>();
pipeline->add(std::make_shared<gr::text_guardrail>(gr::text_guardrail_options {
  .denied_terms = { "prohibited instruction" },
  .redacted_terms = { "internal-token" },
}));

const auto checked = pipeline->evaluate(
  gr::guardrail_stage::input,
  user_input,
  request_id);

if (!checked.allowed()) {
  // Deny the operation or send checked.decision to an approval workflow.
}
```

Use `function_guardrail` to adapt an existing moderation service, jailbreak detector, PII classifier, grounding validator, or application policy. The common request includes text, optional JSON data, stage, subject identifier, and metadata. The result carries structured issues, replacement values, per-check timing, and stable decisions.

## Reasoning integration

`reasoning_runner_options::guardrail_pipeline` applies the pipeline before model execution, around every tool invocation, and to all public output channels. Input denial prevents the model call. Tool-input modification changes the call executed by the tool and the assistant tool call retained in the next model request. Tool-output modification changes the tool message sent to the next model call. Denial stops the loop with a stage-specific reasoning error.

```cpp
reasoning::reasoning_runner runner({
  .client = &client,
  .guardrail_pipeline = pipeline,
});
```

Tool-input requests use the serialized arguments as `content` and expose `tool_call_id` and `tool_name` in `data`. Tool-output requests use the result content, or the stable error message when the result has no content, and expose the same identifiers in `data`. A replacement content value therefore has one unambiguous meaning at each boundary.

When output guardrails are enabled, `buffer_guarded_output` defaults to `true`. Content deltas, reasoning deltas, reasoning summaries, tool-argument deltas, and unvalidated tool-call-ready events are withheld until their applicable boundary has been checked. Sanitized content and reasoning-summary completion events are then emitted. Reasoning-summary and error checks use the output stage with `metadata["channel"]` set to `"reasoning_summary"` or `"error"`. Setting buffering to `false` restores pass-through streaming but means post-generation checks cannot retract data already delivered to the caller.

Reasoning stores decision and timing diagnostics but removes guarded content, replacement values, structured data, issue messages, evidence, remediation, issue metadata, check errors, and custom run metadata by default. Stable issue codes, decisions, stages, guardrail names, timing, and telemetry failure counts remain available. Set `retain_guardrail_evidence = true` only when the host has an appropriate access-control and retention policy.

If an output is modified or blocked, Reasoning also removes auxiliary payloads that could retain the pre-guard value: provider and reasoning metadata, tool calls, reasoning-step outputs, plan outputs and artifacts, reflection records, and trace payloads. A blocked run exposes a fixed public error instead of a custom policy message. The generic planning, retrieval, and memory-write stages remain available for hosts and their module-specific integrations.

## Audit and telemetry

Pipeline options accept the common security `audit_sink`, common observability `event_sink`, and a typed observer. Built-in events contain decision, stage, counts, identifiers, and timing, but never include the guarded content. Telemetry callback failures are isolated by default and counted in `metadata["telemetry_error_count"]`; select `guardrail_telemetry_failure_mode::propagate` when callback failure must abort the caller. Applications remain responsible for ensuring custom issue evidence and metadata are safe when evidence retention is enabled.

The built-in `text_guardrail` is deterministic and intentionally small. `max_characters` counts validated Unicode code points, not UTF-8 bytes or user-perceived grapheme clusters; `max_bytes` provides an independent encoded-size limit. Case-insensitive term matching is ASCII-oriented. It is not a replacement for a model-backed moderation system, semantic jailbreak detection, or jurisdiction-specific compliance policy.
