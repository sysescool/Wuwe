---
id: filesystem-tools
title: Filesystem toolkit
description: Root-scoped, revision-aware filesystem operations for agents.
---

# Filesystem toolkit

The Filesystem module provides structured local file operations without routing
them through a shell. A `filesystem_runtime` owns one policy and one backend;
`filesystem_tool_provider` exposes only the operations enabled by that policy.

```cpp
namespace fs = wuwe::agent::filesystem;

fs::filesystem_policy policy;
policy.root = workspace;
policy.allow_write = true;
policy.require_approval_for_write = true;

fs::filesystem_runtime runtime(
  fs::make_local_filesystem_backend(),
  policy,
  audit_sink,
  approval_service);

fs::filesystem_tool_provider tools(runtime);
```

The default policy permits reads and denies every mutation. Absolute paths and
symbolic-link traversal are disabled by default. Every accepted path is
normalized and checked against the configured root before it reaches the
backend. Results returned to the caller use root-relative paths.

## Operations

The public runtime and model provider cover:

- `file_info`: type, size, and an optional content revision;
- `read_file`: bounded UTF-8 text reads;
- `write_file`: atomic create or replacement;
- `replace_text`: exact replacement with occurrence-count protection;
- `list_directory`: bounded shallow or recursive listing;
- `glob_files`: bounded portable glob traversal, including `**`;
- `search_text`: bounded literal search with independent traversal, input-byte,
  result-count, and output-byte limits;
- `create_directory`;
- `copy_path` and `move_path`;
- `remove_path`, with recursion requested explicitly.

The model-facing tools intentionally operate on UTF-8 text. Binary files can be
inspected with `file_info`, but binary transfer or transformation should use a
product-owned tool with an explicit media contract instead of embedding
unbounded base64 in an LLM tool call.

## Revisions and writes

Text revisions are opaque `sha256:` content identifiers. Supply
`expected_revision` to reject stale writes, replacements, or file removal.
`replace_text` also requires an expected occurrence count unless `replace_all`
is explicit, preventing a vague edit from modifying an unexpected location.

Writes use a temporary file in the destination directory followed by an atomic
replacement operation. `create_new` uses a no-overwrite filesystem primitive,
so it cannot silently replace a file created concurrently. Mutations on one
`local_filesystem_backend` instance are serialized, making revision checks and
their mutation one process-local critical section. A database-backed or remote
workspace should implement `filesystem_backend` with its own transaction or
compare-and-swap contract for cross-process coordination.

The local backend creates temporary files with an exclusive OS primitive,
flushes their contents before replacement, and preserves the destination file's
permissions when replacing an existing regular file. Failures that occur after
creating requested parent directories are marked with `partial=true` and
`parent_directories_created=true`.

Directory copy and recursive removal are bounded by entry counts; copy is also
bounded by total bytes. Recursive copy validates the complete source plan,
including entry type and limits, before creating the destination. A later I/O
failure can still leave already copied entries; failed results preserve
`affected_items`, `bytes_processed`, and a `partial` metadata flag. A directory
cannot be copied or moved into itself, directory moves require explicit
`recursive=true`, and overwrite moves replace regular files only. The configured
root cannot be removed. Hosts that require transactional directory trees should
provide a staging or version-control backend.

Recursive copy rejects symbolic links in both the source plan and every existing
destination component, and rechecks destination components immediately before
each mutation. This prevents an existing link inside an overwrite destination
from redirecting copied content outside that tree. Portable checks still cannot
eliminate races from a hostile concurrent process; use a handle-based or remote
backend for that threat model.

Search is literal rather than regular-expression based. Case-insensitive search
folds ASCII characters; line and column values are one-based, and columns are
UTF-8 byte offsets. `max_files` prevents a small result limit from becoming an
implicit traversal limit, while `max_output_bytes` bounds repeated line text in
the returned result.

## Security boundary

Root checking is a defense-in-depth policy boundary, not an operating-system
sandbox. The local backend rejects configured symlink traversal and obvious
path escapes, but another process with access to the workspace can race portable
path checks. Use an OS sandbox, container, remote workspace service, or a custom
handle-based backend when hostile concurrent filesystem mutation is in scope.

Write, move, and remove approvals are configured separately. Every operation
emits policy and terminal audit events; callback exceptions are contained so a
telemetry failure cannot turn a completed file operation into an ambiguous
retry. Backend exceptions are also converted to audited `io_error` results.
Caller metadata remains extensible, while runtime and backend result fields such
as operation identity, entry type, size, search counters, and partial-mutation
progress are reserved and cannot be overwritten by caller-supplied values.
