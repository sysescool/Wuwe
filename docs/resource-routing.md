---
id: resource-routing
title: Resource-aware routing
description: Select models by capability, quality, latency, token estimates, and remaining cost budget.
---

# Resource-aware routing

The Routing module separates model selection from model execution. A `model_resource_profile` describes one model identifier, its context and output limits, token pricing, normalized quality and latency scores, and supported capabilities. `resource_aware_router` evaluates every registered profile against hard requirements before applying a selection strategy.

```cpp
namespace routing = wuwe::agent::routing;

auto router = std::make_shared<routing::resource_aware_router>();
router->add({
  .model = "economy-model",
  .provider = "openai-compatible",
  .context_window_tokens = 128'000,
  .max_output_tokens = 8'000,
  .input_cost_per_million_tokens = 0.20,
  .output_cost_per_million_tokens = 0.80,
  .quality_score = 0.65,
  .latency_score = 0.90,
  .capabilities = { .tools = true, .streaming = true },
});
router->add({
  .model = "premium-model",
  .provider = "openai-compatible",
  .context_window_tokens = 256'000,
  .max_output_tokens = 32'000,
  .input_cost_per_million_tokens = 2.00,
  .output_cost_per_million_tokens = 8.00,
  .quality_score = 0.95,
  .latency_score = 0.60,
  .capabilities = {
    .tools = true,
    .streaming = true,
    .reasoning = true,
  },
});
```

The `provider` field is a stable runtime binding ID, not necessarily a protocol
name. For example, two OpenAI-compatible endpoints can be registered as
`primary-openai` and `private-gateway` while using the same adapter type.
Profiles are unique by the `(provider, model)` binding, so the same upstream
model can be registered at multiple providers with different pricing, quality,
latency, availability, and capability data. `find(model, provider)` performs an
exact lookup, `find_all(model)` returns every binding, and the compatibility
`find(model)` overload returns the first registered binding for that model.

Both input and output prices must be provided together. Use explicit zero prices for a free or local model. Unpriced models are excluded when pricing is required unless `allow_unpriced_models` is enabled deliberately.

Profiles, request budgets, quality and latency scores, and routing weights must be finite and non-negative within their documented ranges. Extreme but valid token counts and prices saturate at the largest finite representable cost, so candidate sorting and JSON diagnostics never receive `NaN` or infinity.

## Selection contract

`model_route_request` supplies the preferred model, estimated input and output tokens, the remaining per-call cost allowance, and `model_route_requirements`. Hard constraints cover:

- model availability;
- context window and maximum output size;
- tools, streaming, reasoning, JSON response, and local-runtime capabilities;
- minimum quality;
- model pinning through `allow_model_override = false`;
- provider pinning through `allow_provider_override = false`;
- known pricing and estimated cost budget.

Eligible candidates are ranked using `balanced`, `lowest_cost`, `highest_quality`, or `lowest_latency`. The result includes the selected profile, estimated cost, all candidate scores, and stable rejection reasons. No silent fallback occurs when no model satisfies the hard constraints.

## Reasoning integration

Attach a router to `reasoning_runner_options`. Routing runs immediately before every provider call, including later Tool Loop rounds and Reflection retries, so the selected model can change as the remaining budget changes.
An explicit `reasoning_request::provider` is pinned by default. Set
`model_routing.allow_provider_override = true` only when the router is allowed to
send the request to another registered provider. When override is enabled, the
preferred provider receives `preferred_provider_bonus` under the `balanced`
strategy; hard capability and budget constraints still take precedence.

```cpp
reasoning::reasoning_runner runner({
  .client = client.get(),
  .model_router = router,
});

const auto result = runner.run({
  .input = "Analyze the data and use tools when needed.",
  .model = "premium-model", // preferred, not pinned by default
  .policy = {
    .mode = reasoning::reasoning_mode::react,
    .budget = {
      .max_model_calls = 6,
      .max_total_tokens = 20'000,
      .max_cost_usd = 0.25,
      .estimated_output_tokens_per_call = 1'000,
    },
  },
  .model_routing = {
    .strategy = routing::model_selection_strategy::highest_quality,
    .require_tools = true,
  },
});
```

`reasoning_budget` supports separate prompt, completion, total-token, and USD cost limits. Preflight routing uses estimated tokens and remaining cost, and the remaining output allowance is propagated through `llm_request::max_output_tokens` to the built-in OpenAI-compatible, Anthropic, Gemini, and Ollama clients. Provider-reported usage is applied after each call; when detailed usage is absent, the run accounts the preflight estimate and increments `reasoning_usage::estimated_token_calls`. `reasoning_result::model_routes` and `model_routed` trace events expose every decision.

Tool and JSON capabilities are inferred from each concrete model request. Streaming is also required automatically when the configured client supports streaming and the Reasoning policy enables it; custom dispatchers can set `require_streaming` explicitly when their capability cannot be inferred.

The default estimator is deterministic and approximate. Supply `reasoning_runner_options::token_estimator` when the deployed tokenizer or provider has a more accurate implementation.

## Execution boundary

The router selects both `llm_request::model` and `llm_request::provider`. Use
`llm::dispatching_llm_client` when profiles span multiple provider clients:

```cpp
namespace llm = wuwe::agent::llm;

auto clients = std::make_shared<llm::llm_client_registry>();
clients->add({
  .provider = "economy-provider",
  .client = economy_client,
});
clients->add({
  .provider = "premium-provider",
  .client = premium_client,
});

llm::dispatching_llm_client dispatch(clients);
reasoning::reasoning_runner runner({
  .client = &dispatch,
  .model_router = router,
});
```

Every routed request is delivered only to the exact registered provider. An
unknown provider returns `llm_error_code::model_unavailable`; multiple registered
clients without an explicit or configured default provider return
`llm_error_code::invalid_request`. Dispatch never silently falls back across
providers. Wrap individual bindings in `resilient_llm_client` when retry, circuit
breaking, or ordered fallback is required.

The registry is thread-safe. Replacement and removal affect new calls atomically,
while in-flight calls retain shared ownership of the client they selected. The
dispatcher validates the exact selected client's capability contract before
execution and preserves the selected provider and model in response metadata.
Registry mutations reject direct and indirect Wuwe dispatcher ownership cycles
before retaining the new client, so invalid graphs do not create `shared_ptr`
leaks. Runtime dispatch still rejects cycles introduced through custom forwarding
clients with `llm_error_code::invalid_request`. Dispatch stores lineage inside an
opaque request runtime context, so cycles remain detectable across worker-thread
handoffs while an independent request started by a stream callback receives a
fresh context. The context is not application metadata and is never included in
durable continuation serialization. Synchronous cycles encountered during
capability discovery are also contained. Custom forwarding clients that retain
other clients are responsible for avoiding their own strong-ownership cycles.
Stream consumer
exceptions are propagated to the caller after the backend outcome is recorded,
so they are not misclassified as provider failures. A request started by a
dispatch observer is allowed, while events from that nested request are not fed
back into the same observer recursively.

Planning has its own planner client and model configuration. A Reasoning router controls model-driven Reasoning calls; applications that require routed planning should provide a planner whose client performs the corresponding dispatch.

## Telemetry

Router options accept a typed observer and the common `observability::event_sink`. Telemetry failures are isolated and counted by default; `routing_telemetry_failure_mode::propagate` makes them fail the caller. Events contain model identifiers, strategy, candidate count, and stable outcomes, but no prompt content.
