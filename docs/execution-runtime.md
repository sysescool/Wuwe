---
id: execution-runtime
title: Controlled execution
description: Run bounded Python subprocesses under explicit policy, approval, and audit controls.
---

# Controlled execution

The execution module exposes Python snippet execution as a policy-bound runtime and optional model tool. It separates authorization from backend enforcement and reports what each backend actually enforces.

The platform-neutral sandbox policy and compilation boundary is documented in
[Sandbox architecture](sandbox-architecture.md). Execution policy decides whether a request is
authorized; sandbox compilation independently verifies that the selected host backend can enforce
the required isolation before a launch plan exists.

## Request and runtime

```cpp
namespace execution = wuwe::agent::execution;

execution::execution_policy policy;
policy.default_workdir = "work";
policy.max_limits.timeout = std::chrono::seconds(3);
policy.max_limits.max_code_bytes = 64 * 1024;
policy.max_limits.max_stdout_bytes = 64 * 1024;
policy.max_limits.max_stderr_bytes = 64 * 1024;

execution::execution_runtime runtime(
  execution::make_controlled_process_backend(),
  policy);

const auto result = runtime.run({
  .language = execution::execution_language::python,
  .code = "print(6 * 7)",
});
```

`execution_request` carries code, stdin, working directory, limits, environment values, shell intent, and metadata. `execution_result` reports exit status, termination reason, timeout or cancellation, truncation, output, diagnostics, elapsed time, and backend metadata.

Only Python is implemented in 1.0.0.

For arbitrary executables and an explicitly enabled shell adapter, use the
separate [Process toolkit](process-tools.md). It is argv-first and does not
overload Python code execution with command-string semantics.

## Policy

`execution_policy` controls:

- allowed languages;
- default working directory;
- readable and writable roots;
- maximum code, input, output, process, memory, CPU, and elapsed limits;
- network, file-read, file-write, and shell permissions;
- approval requirements for network, writes, and shell use;
- allowed environment values.

`execution_runtime` evaluates and normalizes the request, obtains host approval when required, calls the backend, and emits audit events. A request that requires approval is denied when no approval service is configured.

## Model tool

```cpp
execution::execution_tool_provider tools(runtime);
```

The built-in tool is `run_python_snippet`. Its arguments are bounded separately from the execution request, and its timeout must stay within configured limits. Registering the provider with an agent runner does not weaken runtime policy.

## Backends

### `controlled_process`

This is the default and cross-platform backend. It starts a Python subprocess and provides interpreter probing, working-directory selection, environment filtering, timeouts, cancellation, process-tree cleanup where supported, and bounded stdout and stderr.

It is not a strong sandbox. In particular, policy flags such as network or file denial cannot by themselves prevent an ordinary child process from accessing operating-system resources. Use it only for code trusted to the degree appropriate for a controlled subprocess.

### `restricted_process`

The restricted backend is opt-in:

```cpp
execution::execution_backend_registry_options options;
options.enable_restricted_process_backend = true;
auto registry = execution::make_execution_backend_registry(options);
```

On Windows, an explicitly registered restricted backend is available when its configuration and
compiled policy pass native capability checks. It stages a minimal Python runtime, launches it in an
AppContainer, applies Job Object lifecycle and resource limits, enforces ordered filesystem rules,
and denies network according to its reported enforcement contract.

On macOS, explicit registration compiles the same portable policy into a private, versioned
Seatbelt plan. The launcher uses the SIP-protected `/usr/bin/sandbox-exec` system utility with a
deny-default profile, canonical path validation, denied networking,
an environment allowlist, bounded stdio, a per-process CPU safeguard, and a dedicated process
group. Python source is passed directly with `-c`; no attacker-replaceable script pathname is opened
after policy compilation. Unrelated parent file descriptors are closed before `execve`, and the
profile does not grant a global `mach-lookup` wildcard. Linux remains unavailable.

`restricted_process_sandbox_policy()` maps the existing backend configuration to the portable
`sandbox_policy` contract. `make_restricted_process_sandbox_backend()` compiles it into a private,
versioned Windows or macOS plan. `create_restricted_process_backend(compiled.plan)` returns a typed creation
result and rejects null, generic, foreign, or stale plans. The compatibility
`make_restricted_process_backend(config)` API goes through the same compiler; it is not a separate
configuration-only launch path.

Policy CPU, memory, and process limits cap the values in each execution request. Requested working
directories must already be writable under the compiled policy. Parent environment inheritance is
performed only when the policy explicitly requests it; otherwise only explicit UTF-8 variables and
the minimal Windows bootstrap environment are supplied.

Windows policy roots must exist on a persistent-ACL filesystem and must not traverse reparse points.
Windows AppContainer/application-package resource grants are reported as intrinsic platform defaults;
policies with `filesystem.include_platform_defaults=false` fail closed with
`filesystem_platform_defaults_required`. Explicit protected and denied paths remain authoritative,
including over broad `ALL APPLICATION PACKAGES` grants. ACL grants are leased for one execution,
restored with bounded retries afterward, and process-wide serialized to prevent overlapping
executions from corrupting one another's DACL snapshots. Profile deletion is explicit and reported
as `appcontainer_profile_cleanup_status`. Full filesystem reads,
unrestricted/filtered networking, and local binding currently fail compilation because this backend
cannot preserve those semantics exactly. Linux remains unavailable; macOS rejects filtered
networking. macOS applies `RLIMIT_CPU` to each process as defense in depth, but does not advertise
that as an aggregate execution CPU limit. Per-process-tree count, aggregate CPU, and a complete memory
ceiling remain explicitly unavailable until a separately governed helper can provide stronger process
governance. (`RLIMIT_AS` and `RLIMIT_DATA` are rejected by the supported macOS runtime and are not
advertised as enforcement.)

Container and WebAssembly backends are not implemented.

## Backend selection

`execution_backend_registry` describes backends and selects one only when its `sandbox_enforcement_contract` satisfies the requested requirements. Inspect `sandbox_backend_info` instead of assuming that a backend name implies a particular control.

The package smoke test verifies the default controlled backend, explicit restricted-backend registration behavior, and installed-package execution APIs on Windows, macOS, and Linux.
