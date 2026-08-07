---
id: process-tools
title: Process toolkit
description: Allowlisted argv execution and an explicit high-risk shell adapter.
---

# Process toolkit

The Process module executes a program from a host-owned allowlist. Its primary
API is argv-based: the executable and every argument remain separate values and
are never reconstructed through a shell.

```cpp
namespace proc = wuwe::agent::process;

proc::process_policy policy;
policy.working_directory_root = workspace;
policy.allowed_executables = { compiler_path, scanner_path };
policy.allowed_environment_overrides = { "BUILD_MODE" };
policy.max_limits.timeout = std::chrono::seconds(30);

proc::process_runtime runtime(
  proc::make_local_process_backend(),
  policy,
  audit_sink,
  approval_service);

const auto result = runtime.run({
  .executable = compiler_path,
  .arguments = { "--version" },
});
```

Absolute allowlist entries authorize exactly that normalized executable. A bare
filename authorizes lookup only through `executable_search_paths`; it cannot
authorize an arbitrary absolute path with the same filename. Relative paths
with directory components are rejected in the allowlist to keep this distinction
unambiguous.

Working directories are normalized under `working_directory_root`. Environment
inheritance is disabled by default. The host supplies a base environment and an
explicit set of keys that a request may override. Arguments, stdin, stdout,
stderr, elapsed time, argument counts, and explicit environment size/count all
have policy ceilings. Request-level ceilings are enforced as well as policy
ceilings; zero-valued request limits select the policy maximum rather than
disabling the bound. Invalid or platform-equivalent duplicate environment keys
are rejected during policy construction.

`default_workdir` is normalized and validated when `process_runtime` is
constructed. A missing or out-of-root default is a configuration error rather
than a deferred per-request policy denial.

## Model tools

`process_tool_provider` exposes `run_process` when at least one executable is
allowlisted. The result contains termination reason, exit code, process ID,
stdout, stderr, truncation flags, elapsed time, and backend metadata. A non-zero
program exit remains a successfully delivered process result rather than a tool
transport failure.

`run_shell` is a separate adapter. It appears only when both
`process_policy::allow_shell` and `process_tool_options::expose_shell_tool` are
true. Shell execution uses the platform shell configured by the host, is marked
as the critical `process.shell` capability, and requires approval by default.
Applications should prefer `run_process` whenever a command can be represented
as an executable and argument vector.

## Local backend

The built-in local backend provides:

- direct process creation without an intermediate shell;
- strict Windows argument quoting and POSIX `execve` argv delivery;
- bounded stdin, stdout, and stderr handling;
- cooperative cancellation and wall-clock timeout;
- process-tree cleanup using a Windows Job Object or a POSIX process group;
- restricted inherited handles and an explicit environment block;
- cleanup of descendants that retain output pipes after the root exits.

The default `use_process_tree=true` gives the runtime ownership of descendants.
If a host explicitly disables it, timeout and cancellation still bound the root
process and local pipe workers, but descendant cleanup is reported as not
enforced and descendants may outlive the request. Platform command-line and
environment-block limits are checked before Windows process creation.

The backend is a controlled process, not a strong sandbox. It does not itself
deny filesystem or network access and reports those enforcement levels as not
enforced. Run untrusted commands in a restricted, container, or remote backend
that implements the same `process_backend` interface.

Approval, backend, and audit callback failures are contained and returned as
typed terminal outcomes. Cancellation, timeout, launch failure, policy denial,
approval denial, backend failure, and an ordinary non-zero exit remain distinct.
Operation identity, resolved executable, shell mode, launch diagnostics, and
signal metadata are runtime-owned fields and cannot be spoofed through caller
metadata.
