---
id: storage-contracts
title: Storage contracts
description: Inspect persistence, transaction, migration, and coordination guarantees.
---

# Storage contracts

`core::storage_capabilities` is the common capability vocabulary used by built-in
Run, Memory, Knowledge Store, and Knowledge Index implementations. It reports:

- durability and transaction support;
- atomic mutation guarantees;
- optimistic concurrency and ordered replay where applicable;
- schema migration and active schema version;
- process-local, single-node, or distributed coordination;
- multi-process safety.

Third-party adapters remain source-compatible across Wuwe 1.x after recompilation:
their default capability contract is undeclared. Adding virtual methods changes
the C++ ABI, so Wuwe does not promise cross-release binary compatibility unless a
release states otherwise. New adapters should return `declared = true` and call
`validate_storage_capabilities()` in their tests.
Declaring distributed coordination without optimistic concurrency, migrations
without a version, or transactions without atomic mutations fails validation.

## Built-in guarantees

| Adapter | Durable | Atomic/transactional | Schema migration | Coordination |
| --- | ---: | --- | --- | --- |
| In-memory stores/indexes | No | Process mutex | No | Process-local |
| File stores/indexes | Yes | Not declared | No | Process-local |
| SQLite Memory Store | Yes | Transactional | Versioned | Single node, multi-process |
| SQLite Knowledge Index | Yes | Transactional | Versioned | Single node, multi-process |
| SQLite Agent Run Store | Yes | Record + event transaction | Versioned | Single node, multi-process |

File adapters are intentionally honest: they are useful local persistence but do
not claim multi-process coordination, transactional replacement, or migration
semantics they cannot guarantee.

SQLite adapters use WAL, a busy timeout, immediate migration transactions, an
explicit component version, ordered migrations, and startup validation of required
columns and primary keys. An unversioned legacy table layout migrates without
discarding records; Memory Store version 2 adds a transactional identifier sequence,
while Knowledge Index and Agent Run Store currently use version 1. A database
created by a newer Wuwe version, or an existing table whose shape contradicts its
declared version, is rejected instead of being opened with unknown semantics.

For distributed deployments, implement the relevant abstract Store interface over
the application's database and declare distributed coordination only when atomic
compare-and-swap semantics are actually enforced.

The general compatibility contract is documented in
[Versioning and compatibility](versioning.md).
