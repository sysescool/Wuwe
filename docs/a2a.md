---
id: a2a
title: Agent2Agent protocol
description: Discover remote agents and exchange Agent Cards, Messages, Tasks, Artifacts, and cancellation requests over A2A.
---

# Agent2Agent protocol

The A2A module connects remote agents without treating MCP as an agent-to-agent protocol. It provides Agent Card discovery, typed Message/Part/Task/Artifact models, JSON codecs, JSON-RPC methods, in-process and HTTP transports, client and service boundaries, and adapters for the Multi-Agent runtime.

A2A Agent Card skills are protocol advertisements. They are not equivalent to a
locally verified [Wuwe Skill package](skills.md). The Skills adapter can publish a
bounded descriptor and Wuwe version/digest extension, but a descriptor received
from a remote card remains `retrieved_untrusted` and non-activatable until a host
obtains and verifies a package through a separate supply-chain process.

## Supported protocol surface

The public types model the A2A `0.3.0` data shape and these JSON-RPC operations:

- `message/send`;
- `tasks/get`;
- `tasks/cancel`.

Text, structured data, URI files, and base64 file parts are represented explicitly. Stable A2A and JSON-RPC error codes survive the service, transport, and client layers.

JSON-RPC notifications are executed without a response; the HTTP adapter returns `204 No Content`. Request IDs and object-shaped parameters are validated, and output-mode negotiation fails with `content_type_not_supported` when the client's accepted modes have no intersection with the Agent Card defaults.

The synchronous `service` does not implement streaming or push notification methods and rejects Agent Cards that advertise those capabilities. This keeps discovery truthful. `transport_capabilities::concurrent_invocation` is conservative by default; a remote Multi-Agent executor advertises concurrent execution only when its transport explicitly declares thread-safe concurrent calls.

## Remote discovery and calls

```cpp
namespace a2a = wuwe::agent::a2a;
namespace ma = wuwe::agent::multi_agent;

auto transport = std::make_shared<a2a::http_client_transport>(
  a2a::http_client_transport_options {
    .endpoint = "https://agents.example/a2a",
    .agent_card_url =
      "https://agents.example/.well-known/agent-card.json",
    .headers = { { "Authorization", "Bearer ..." } },
  });

a2a::client client(transport);
const auto card = client.discover();
const auto task = client.send({
  .value = {
    .message_id = "message-1",
    .parts = { a2a::part::text_part("Review this draft") },
    .task_id = "task-1",
    .context_id = "conversation-1",
  },
});
```

If `agent_card_url` is omitted, the HTTP transport derives `/.well-known/agent-card.json` from the endpoint origin. JSON-RPC version and response IDs are validated. The common HTTP client currently provides cancellation checks before and after a request; transports advertise whether they can cancel an in-flight operation.

## Non-blocking tasks

Set `send_message_configuration::blocking = false` when using `team_task_handler`. The service immediately returns a submitted Task, executes the local team in the background, exposes state through `tasks/get`, and forwards `tasks/cancel` to a cooperative `stop_token`.

`team_task_handler_options::max_background_tasks` bounds in-process background work and must be greater than zero. Capacity exhaustion returns an `internal_error` with structured `error.data` containing `reason = "background_capacity_exhausted"`, `retryable = true`, and the configured maximum. Background launch failures roll back the tentative task record and are returned through the same typed result boundary rather than escaping as thread exceptions.

Destroying a handler requests cancellation for every accepted background task and
waits for its cooperative workers to finish before releasing the task registry.
Agent executors should honor their `stop_token`; an uncooperative executor can
necessarily delay shutdown because Wuwe does not terminate application threads.

Once `tasks/cancel` succeeds, cancellation remains terminal even if the worker returns concurrently. Message and request metadata are merged into the local `agent_task_request`; request-level keys override message-level keys, strings remain strings, and other JSON values use their serialized representation.

An `input-required` Task can be continued by sending another Message with the same `taskId`. The bridge inherits the existing context when `contextId` is omitted, appends history, clears the previous status message after successful continuation, and replaces Artifacts with matching IDs. Paused tasks may also be cancelled without holding an Agent capacity lease.

Background task storage is in-memory. Production deployments that need durable task recovery, multiple service instances, or exactly-once processing should implement `task_handler` on their database and queue infrastructure.

## Multi-Agent bridge

Team integration is optional. Include `<wuwe/agent/a2a/multi_agent_adapter.hpp>` explicitly; the core A2A umbrella does not pull the Multi-Agent and Planning dependency chain into protocol-only translation units.

`remote_agent_executor` makes an A2A client usable anywhere a local `agent_executor` is accepted. `agent_descriptor_from_card()` maps remote skills into registry skills, and Task and Artifact metadata are preserved as local string metadata (non-string JSON values are serialized). In the other direction, `team_task_handler` exposes a local `team_runtime` through A2A Task semantics.

```cpp
const auto discovered = client.discover();
auto registry = std::make_shared<ma::agent_registry>();
registry->add(
  a2a::agent_descriptor_from_card(*discovered.value, "remote-reviewer"),
  std::make_shared<a2a::remote_agent_executor>(
    std::make_shared<a2a::client>(transport)));
```

`http_service_adapter` converts framework-neutral HTTP requests into Agent Card or JSON-RPC responses. It does not start a listener. The host chooses the HTTP server, route mounting, TLS, authentication, authorization, rate limits, request-size limits, audit policy, and tenant isolation.

## Protocol boundary

A2A carries agent tasks and artifacts. MCP carries tools, prompts, and contextual resources. An application may expose both, but discovery, authorization, lifecycle, and error semantics remain separate.
