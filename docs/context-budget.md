---
id: context-budget
title: Context budget
description: Allocate a model context window across prompts, conversation, memory, retrieval, tools, and output.
---

# Context budget

`llm_context_budget` gives one request-level policy authority over the complete
model context. It accounts for system messages, conversation, Skills, Memory, Knowledge,
tool schemas, tool-result exchanges, other messages, and reserved model output.
Memory and Knowledge mark their injected messages explicitly; ordinary requests
can set `chat_message::context_source` when they add another context component.

```cpp
wuwe::llm_request request;
request.model = "production-model";
request.context_budget = wuwe::llm_context_budget {
  .context_window_tokens = 128000,
  .reserved_output_tokens = 4096,
  .minimum_recent_conversation_messages = 4,
  .limits = {
    .system = 12000,
    .conversation = 60000,
    .skills = 12000,
    .memory = 12000,
    .knowledge = 24000,
    .tool_schemas = 12000,
    .tool_results = 16000,
  },
  .minimum_recent_tool_exchanges = 1,
};

const auto response = runner.complete(std::move(request));
```

The agent runner applies the budget after `prepare_model_request` and before
every provider call, including later tool-loop rounds. This ensures host-added
context and accumulated tool results are included. A budget failure returns
`llm_error_code::context_budget_exceeded` before the provider receives the
request.

The default policy preserves system messages and complete tool schemas. It first
reduces Memory, Knowledge, Skills, old tool exchanges, old conversation, and other
context. The configured number of newest conversation messages is neither
dropped nor truncated. Tool calls and their matching results are budgeted as
atomic exchanges rather than individual strings. The configured number of newest
tool exchanges is preserved byte-for-byte; older exchanges can only be removed as
a whole. This prevents context fitting from creating orphan calls, invalidating a
structured Tool projection, or losing its error identity. Set
`minimum_recent_tool_exchanges` to zero only when the host explicitly permits all
Tool history to be removed. Set `allow_system_truncation` only when the host has
designed its system prompt to tolerate truncation.

Skill adapters set `chat_message::context_source` to
`llm_context_source::skill`. This keeps reusable instructions visible in
`context_budget_usage::skills` and independently bounded by `.limits.skills`
instead of hiding them inside the general system or conversation totals.

`context_budget_manager` is independently usable when a host assembles requests
outside `llm_agent_runner`. Its result contains before/after component usage,
dropped and truncated message counts, and a stable failure reason.
`llm_agent_callbacks::on_context_budget` observes the successful report for every
model round without changing the request.

New tool results pass through the runner's `tool_output_projection_policy` before
they enter the next model request. That policy is a per-result hard ceiling and
preserves the full outcome outside the model boundary. Context budgeting runs later
over the complete request and may still remove older Tool exchanges atomically to
fit the shared window. It never applies generic string truncation to a Tool result.
Use projection to prevent one Tool result from dominating a round; use the context
budget to allocate space across all context components.

Tokenization is injectable through `context_token_estimator`. Wuwe's default
estimator is a conservative UTF-8-aware heuristic suitable for portable fallback
behavior. Production integrations with provider tokenizers should inject an exact
model-specific estimator through `llm_agent_run_options::token_estimator`.
`context_token_estimator` extends the lightweight `text_token_estimator` interface
with message and Tool-schema accounting; model-output projection depends only on
the text interface and does not pull Context Budget or message types into its
public declaration header.

All component aggregation is saturating. A custom estimator that reports values
near `std::numeric_limits<std::size_t>::max()` therefore produces a conservative
budget rejection instead of wrapping to a small token count.

The budget does not silently delete tools to make a request fit. Tool selection is
a routing or application-policy decision because removing a schema changes which
actions the model can perform.
