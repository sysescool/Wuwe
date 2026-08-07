---
id: sandbox-architecture
title: Sandbox architecture
description: Define portable sandbox policy, compile it against explicit backend enforcement, and fail closed before launch.
---

# Sandbox architecture

Wuwe separates portable security intent from platform-specific enforcement. A host describes the sandbox it requires with `sandbox_policy`; a platform backend compiles that policy into an immutable, backend-bound `sandbox_plan`. Compilation succeeds only when the backend reports every required control as `enforced`.

Phase one established the portable contract. Phase two connects the Windows restricted-process
launcher to a native, versioned plan produced by that contract. macOS compiles the same contract to
a deny-default Seatbelt plan executed by the SIP-protected system sandbox utility; Linux remains unavailable
until their native backends implement the same compiler and execution boundary.

## Layers

```text
execution policy and approval
            |
            v
      sandbox_policy
            |
            v
 sandbox_backend::compile()
            |
       +----+----+
       |         |
 sandbox_plan   typed failure + blockers
       |
 platform launch implementation
```

Authorization remains above the sandbox. Approval decides whether an operation may be attempted; sandbox compilation decides whether the host can technically enforce the approved policy. Approval never turns partial or missing enforcement into a valid plan.

## Portable policy

`sandbox_policy` contains only cross-platform security decisions:

- required isolation level;
- full or restricted filesystem read access;
- readable, writable, protected-read-only, and denied paths;
- denied, unrestricted, or filtered networking;
- filtered-network default action, ordered host rules, and optional ports;
- parent-environment inheritance and explicit environment values;
- process-tree cleanup, process count, memory, and CPU constraints;
- field-level enforcement requirements;
- host metadata that does not grant authority.

`sandbox_backend::probe()` returns `sandbox_host_capabilities`, combining the current host platform,
backend availability, maximum enforcement contract, and structured availability blockers. Hosts can
inspect this before policy compilation, but compilation remains authoritative because a generally
available backend may still be unable to enforce a particular policy.

All filesystem roots in the portable contract are absolute. `validate_sandbox_policy()` rejects relative or empty roots, invalid environment entries, ambiguous network rules, and zero-valued resource limits. It also performs lexical path normalization and stable exact deduplication. This is configuration normalization, not a substitute for handle-based platform enforcement against symlink, junction, mount, or TOCTOU attacks.

Path policy precedence for platform backends is:

```text
denied > protected read-only > writable > readable > platform default
```

A backend must refuse a policy when it cannot preserve this precedence.

## Network semantics

Network modes are intentionally distinct:

- `denied` requires complete outbound network denial;
- `unrestricted` requests no sandbox network restriction;
- `filtered` requires both direct-network denial and an enforced filtering path.

`network_filter` is separate from `network_deny` in the enforcement contract. A backend that can only disable all networking cannot claim support for filtered networking. No current Wuwe backend reports `network_filter` as enforced.

Filtered rules have deterministic, platform-independent evaluation semantics:

1. Rules are evaluated in declaration order; the first matching rule wins.
2. If no rule matches, `default_action` applies. Its default is `deny`.
3. A missing port matches every port. A configured port matches only that port.
4. `*` matches every host. `*.example.com` matches exactly one label below the suffix, while
   `**.example.com` matches the suffix itself and any subdomain depth.
5. DNS matching is ASCII case-insensitive; IP literals are matched exactly. Patterns contain a host
   only, never a URL, path, query, or fragment.

Backends must reject a filtered policy if they cannot preserve this ordering and matching behavior.
For `denied` and `unrestricted` modes, rules are forbidden and `default_action` remains `deny` so
that ignored filter configuration cannot silently change meaning.

## Compilation and plans

```cpp
auto backend = execution::make_restricted_process_sandbox_backend(config);
auto policy = execution::restricted_process_sandbox_policy(config);
auto compiled = backend->compile(policy);

if (!compiled) {
  // Inspect compiled.error and compiled.blockers. Do not launch unsandboxed.
}

auto created = execution::create_restricted_process_backend(compiled.plan);
if (!created) {
  // Reject null, foreign, incompatible, or stale plans.
}
```

Compilation performs four gates in order:

1. validate and normalize the policy;
2. verify backend availability;
3. verify the required isolation level;
4. verify every explicit and policy-implied enforcement requirement.

Successful plans retain the normalized policy, backend identity, host platform, and actual enforcement contract. A plan is evidence produced by a backend compiler, not a portable authorization token. The Windows launcher accepts only its private `windows_restricted_process` plan type and validates its format version before creating an execution backend. Generic logical plans, null plans, plans from another platform, and stale native plans are rejected.

`sandbox_policy_to_json()` and `sandbox_policy_from_json()` provide a versioned schema for durable host configuration. Plan objects are deliberately not serializable because they may contain platform-specific, process-local enforcement state.

## Compatibility adapter

The existing `restricted_process_backend_config` remains source-compatible. `restricted_process_sandbox_policy()` maps it to the portable policy, and `make_restricted_process_sandbox_backend()` exposes compilation independently from Python execution.

The compatibility factory still accepts `restricted_process_backend_config`, but it now compiles the
compatibility policy and creates the same native plan used by the explicit API. There is no separate
configuration-only launch path. On macOS it creates the same native Seatbelt plan as the explicit
API. On Linux it fails closed with `restricted_process_unsupported_platform`; it does not
manufacture a logical plan for an unavailable backend.

## Windows native plan

The Windows compiler binds policy intent to executable platform state:

- resolves and validates the configured Python interpreter;
- verifies that policy roots exist, contain no reparse-point components, and reside on a volume
  with persistent ACL support;
- rejects full filesystem reads, unrestricted or filtered networking, and local binding because the
  current AppContainer launcher cannot preserve those semantics exactly;
- requires `filesystem.include_platform_defaults=true` and records
  `filesystem_platform_defaults=windows_appcontainer_intrinsic`, because Windows resources granted
  to AppContainer/application-package groups are intrinsic platform defaults that this launcher
  cannot globally subtract;
- normalizes Windows environment handling, rejects case-insensitive name collisions, and validates
  UTF-8 before creating a Unicode environment block;
- applies policy process, memory, and CPU limits as upper bounds on each execution request;
- stores a private plan format version and process-local plan identity.

At execution time, filesystem rules are applied in the documented precedence order. Writable roots
receive only data read/write/create and self-delete rights; the sandbox never receives `WRITE_DAC`,
`WRITE_OWNER`, or parent `FILE_DELETE_CHILD` authority. Protected and denied descendants use
protected DACLs so inherited writable access cannot override them.
For protected and denied paths, Wuwe also masks pre-existing allow ACEs for the Windows application
package groups and installs explicit deny ACEs for the concrete AppContainer and both application
package groups. This keeps explicit path policy authoritative even when a host tree was broadly
granted to `ALL APPLICATION PACKAGES`, while preserving unrelated host ACEs for restoration.

ACL changes are temporary leases. Before modification, Wuwe records each original DACL and its
inheritance state. After execution it removes the AppContainer SID from newly created descendants
and restores every original descriptor. Failed scope or descriptor restores remain in the lease and
are retried up to three times; recovery state is not discarded by a failed attempt. AppContainer
profile deletion is an explicit, reported cleanup step and is attempted even after ACL cleanup
failure. Overlapping Windows ACL leases are process-wide serialized;
this deliberately trades throughput for deterministic cleanup until a path-scoped lock manager is
introduced.

The Windows path boundary opens the absolute root and every existing ancestor one component at a
time, rejects reparse points by handle, and retains non-share-delete handles until the leaf operation
finishes. ACL mutation/restoration, runtime staging reads and writes, and request workspace creation
reuse this module. Request root and script locks remain live through process execution so their
identity cannot be replaced between staging and launch. Existing files with multiple hard links are rejected before ACL access is granted,
because an ACL grant applies to the file object and would otherwise be reachable through aliases
outside the declared root. Leaf handles are opened with `FILE_FLAG_OPEN_REPARSE_POINT`.

## Platform roadmap

Later phases implement the same contract with different mechanisms:

- Windows: native plan execution is implemented for restricted reads, ordered read/write/deny rules,
  AppContainer network denial, environment policy, Job Object lifecycle, and resource limits;
- Linux: Bubblewrap, seccomp, Landlock compatibility paths, and resource-control integration;
- macOS: implemented Seatbelt filesystem/network isolation, process-group lifecycle, and
  a per-process `RLIMIT_CPU` safeguard; source execution has no temporary-script path lookup,
  unrelated descriptors are closed before `execve`, and the
  Seatbelt profile has no global Mach-service wildcard. A complete
  virtual-memory ceiling and per-tree process-count limit remain explicitly unavailable;
- all platforms: a separately enforced network broker for filtered outbound access.

Platform mechanisms may differ, but enforcement reporting and failure semantics must remain identical.
