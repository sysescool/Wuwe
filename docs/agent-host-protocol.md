---
id: agent-host-protocol
title: Agent Host protocol
description: Expose Wuwe runs through a versioned, transport-neutral service contract.
---

# Agent Host protocol

The Agent Host protocol is the stable boundary between Wuwe's C++ runtime and an
application-owned HTTP server, sidecar, IPC bridge, or in-process service. It is
not an HTTP server and does not impose routes, authentication, deployment, or UI.

The public API is split into three layers:

- `host_protocol.hpp` defines the versioned envelope, operations, negotiation,
  correlation, idempotency, and structured errors;
- `host_service.hpp` defines typed service requests and results;
- `host_dispatcher.hpp` validates and maps JSON envelopes to an
  `agent_host_service` implementation.

This keeps Wuwe Core independent of a product while allowing non-C++ consumers to
share one wire contract.

## Operations

| Operation | Typed request | Purpose |
| --- | --- | --- |
| `create_run` | `create_run_request` | Submit a caller-identified run and input |
| `get_run` | `get_run_request` | Read the public run view |
| `cancel_run` | `cancel_run_request` | Cancel using an expected revision |
| `resolve_approval` | `resolve_approval_request` | Approve or deny a suspended run |
| `resume_run` | `resume_run_request` | Reclaim an approved continuation |
| `list_events` | `list_events_request` | Read events after an exclusive sequence cursor |

Mutating operations require an idempotency key. Revision-bearing operations use
optimistic concurrency. `create_run` requires a caller-assigned `context.run_id`,
so retry and correlation do not depend on a process-local generated identifier.

The dispatcher verifies that a required idempotency key is present; the Service
owns its durable semantics. Keys should be scoped by authenticated principal and
operation. Repeating the same key with the same canonical request must replay the
original outcome, while reusing it with a different request must return `conflict`.
A durable Service must persist the idempotency record and state mutation atomically.

## Envelope

```json
{
  "protocolVersion": "2026-07-01",
  "requestId": "request-42",
  "idempotencyKey": "operation-42",
  "operation": "cancel_run",
  "metadata": {},
  "body": {
    "runId": "run-42",
    "expectedRevision": 7,
    "reason": "cancelled by operator"
  }
}
```

`host_request_from_json()` rejects a missing or unsupported version, missing
correlation, missing mutation idempotency, unknown operation, malformed body, and
invalid revision before calling application code. `host_response_from_json()`
performs the corresponding client-side validation.

`host_dispatcher::dispatch_json()` is exception-safe. Validation errors become
`invalid_request`; unsupported versions report the supported set; service errors
such as `not_found`, `conflict`, or `unavailable` remain typed. Unexpected
exceptions are converted to a generic `internal` error rather than exposing
implementation details.

## Service implementation

Applications implement `agent_host_service`. The implementation can compose
`agent_run_runtime`, an LLM runner, a queue, a database adapter, and product policy.
The protocol does not own authentication or authorization: authenticate the
transport first and authorize each typed operation in the service.

Runtime continuation tokens are bearer secrets and never cross the Host protocol.
`get_run` returns `run_view`, which omits continuation payloads, admitted tool-result
state, and internal idempotency data. `resolve_approval` and `resume_run` identify a
pending action by `approvalId`; after authorization, the Service maps that identifier
to the internal continuation token. Keep tokens out of logs, event attributes,
execution-context metadata, and model-visible content. Wuwe's execution-context
serialization and projection omit sensitive metadata names by default, and durable
run creation rejects them.

`host_dispatcher` is stateless after construction and may be called concurrently.
Its referenced `agent_host_service` must therefore be thread-safe or serialize its
own operations. Response `result` values and event `data` are intentionally
application-visible extension points; the Service must sanitize them before return
and must not place credentials, continuation data, or internal authorization state
inside them. The same rule applies to typed error messages, details, and metadata;
the dispatcher additionally replaces `internal` Service error text, details, and
metadata with a generic public error.

## Event streaming

`list_events(run_id, after_sequence)` is the protocol primitive for polling and
SSE reconnect. An HTTP adapter can use the event sequence as the SSE `id`, resume
from `Last-Event-ID`, and call `list_events` again when `hasMore` is true. The
dispatcher validates limits and rejects event pages whose cursor moves backwards
or skips records, whose events are not strictly ordered, or whose run identity does
not match the request. A non-empty page returns the last event sequence as its next
exclusive cursor; an empty page preserves the requested cursor and cannot claim
that more events are immediately available.

Wuwe deliberately does not define an authentication scheme or HTTP route layout.
Those are deployment concerns; the protocol DTOs and error semantics remain the
same for REST, JSON-RPC, named pipes, or an embedded host.
