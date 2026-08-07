#include "restricted_process_execution_plan_macos.hpp"

#ifdef __APPLE__

#include <algorithm>
#include <string_view>
#include <wuwe/agent/process/local_process_backend.hpp>

namespace wuwe::agent::execution::detail {
namespace {

execution_result convert_result(process::process_result source) {
  execution_result result {
    .exit_code = source.exit_code,
    .stdout_truncated = source.stdout_truncated,
    .stderr_truncated = source.stderr_truncated,
    .stdout_text = std::move(source.stdout_text),
    .stderr_text = std::move(source.stderr_text),
    .error_message = std::move(source.error_message),
    .elapsed = source.elapsed,
    .metadata = std::move(source.metadata),
  };
  switch (source.termination_reason) {
    case process::process_termination_reason::exited:
      result.termination_reason = execution_termination_reason::exited;
      break;
    case process::process_termination_reason::timed_out:
      result.termination_reason = execution_termination_reason::timeout;
      result.timed_out = true;
      break;
    case process::process_termination_reason::cancelled:
      result.termination_reason = execution_termination_reason::cancelled;
      result.cancelled = true;
      break;
    case process::process_termination_reason::policy_denied:
      result.termination_reason = execution_termination_reason::policy_denied;
      break;
    case process::process_termination_reason::launch_failed:
      result.termination_reason = execution_termination_reason::launch_failed;
      break;
    default:
      result.termination_reason = execution_termination_reason::backend_error;
      break;
  }
  result.metadata["sandbox_backend"] = "macos_seatbelt";
  result.metadata["process_tree_cleanup_enforcement"] = "enforced";
  return result;
}

bool path_within(const std::filesystem::path& root, const std::filesystem::path& path) {
  auto root_it = root.begin();
  auto path_it = path.begin();
  while (root_it != root.end() && path_it != path.end() && *root_it == *path_it) {
    ++root_it;
    ++path_it;
  }
  return root_it == root.end();
}

bool valid_environment(const std::map<std::string, std::string>& environment) {
  constexpr std::size_t max_entries = 256;
  constexpr std::size_t max_bytes = 64 * 1024;
  if (environment.size() > max_entries) return false;
  std::size_t total = 0;
  for (const auto& [name, value] : environment) {
    if (name.empty() || name.find('=') != std::string::npos ||
        name.find('\0') != std::string::npos || value.find('\0') != std::string::npos)
      return false;
    if (name.size() > max_bytes - (std::min)(total, max_bytes) ||
        value.size() > max_bytes - (std::min)(total + name.size(), max_bytes))
      return false;
    total += name.size() + value.size() + 2;
    if (total > max_bytes) return false;
  }
  return true;
}

} // namespace

execution_result run_macos_restricted_execution_plan(
  std::shared_ptr<const macos_restricted_process_sandbox_plan> plan,
  const execution_request& request, std::stop_token stop_token) {
  if (!plan || request.language != execution_language::python || request.use_shell) {
    return { .termination_reason = execution_termination_reason::policy_denied,
      .error_message = "macOS restricted_process accepts only direct Python execution" };
  }
  if (request.code.size() > request.limits.max_code_bytes ||
      request.stdin_text.size() > request.limits.max_stdin_bytes ||
      request.code.size() + request.stdin_text.size() > request.limits.max_total_input_bytes) {
    return { .termination_reason = execution_termination_reason::policy_denied,
      .error_message = "execution input exceeds configured limits" };
  }

  const auto& config = plan->runtime_config();
  auto workdir = request.workdir.empty() ? config.fallback_workdir : request.workdir;
  std::error_code error;
  workdir = std::filesystem::canonical(workdir, error);
  if (error || std::none_of(config.writable_roots.begin(), config.writable_roots.end(),
                 [&](const auto& root) { return path_within(root, workdir); })) {
    return { .termination_reason = execution_termination_reason::policy_denied,
      .error_message = "working directory is outside writable sandbox roots" };
  }

  std::map<std::string, std::string> environment = config.base_environment;
  for (const auto& [name, value] : request.env) environment.insert_or_assign(name, value);
  if (!valid_environment(environment))
    return { .termination_reason = execution_termination_reason::policy_denied,
      .error_message = "execution environment is invalid or exceeds macOS sandbox limits" };
  std::vector<std::string> arguments { "-p", plan->seatbelt_profile(), "/usr/bin/env", "-i" };
  arguments.reserve(arguments.size() + environment.size() + 3);
  for (const auto& [name, value] : environment) arguments.push_back(name + "=" + value);
  arguments.push_back(config.python_interpreter.string());
  arguments.push_back("-c");
  arguments.push_back(request.code);
  process::process_request launch {
    .executable = config.seatbelt_executable,
    .arguments = std::move(arguments),
    .stdin_text = request.stdin_text,
    .workdir = workdir,
    .environment = {},
    .inherit_parent_environment = false,
    .limits = {
      .timeout = request.limits.timeout,
      .max_stdout_bytes = request.limits.max_stdout_bytes,
      .max_stderr_bytes = request.limits.max_stderr_bytes,
      .max_stdin_bytes = request.limits.max_stdin_bytes,
      .max_cpu_time = request.limits.max_cpu_time,
    },
  };
  process::local_process_backend backend({ .use_process_tree = config.use_process_group });
  auto result = convert_result(backend.run(launch, stop_token));
  result.metadata["code_transport"] = "argv_no_path_lookup";
  result.metadata["sandbox_launcher"] = "apple_sandbox_exec";
  result.metadata["bootstrap_environment"] = "empty";
  result.metadata["cpu_time_limit_enforcement"] = "per_process_rlimit_only";
  result.metadata["max_cpu_time_ms"] = std::to_string(request.limits.max_cpu_time.count());
  return result;
}

} // namespace wuwe::agent::execution::detail
#endif
