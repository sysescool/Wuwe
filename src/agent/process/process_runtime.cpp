#include <wuwe/agent/process/process_runtime.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <wuwe/agent/capability/capability.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wuwe::agent::process {
namespace {

void safely_publish(audit::audit_sink* sink, const audit::audit_event& event) noexcept {
  if (!sink) return;
  try { sink->publish(event); }
  catch (...) {}
}

bool same_path(
  const std::filesystem::path& left,
  const std::filesystem::path& right) {
#ifdef _WIN32
  auto left_text = left.wstring();
  auto right_text = right.wstring();
  std::transform(left_text.begin(), left_text.end(), left_text.begin(), ::towlower);
  std::transform(right_text.begin(), right_text.end(), right_text.begin(), ::towlower);
  return left_text == right_text;
#else
  return left == right;
#endif
}

bool path_within(
  const std::filesystem::path& candidate,
  const std::filesystem::path& root) {
  auto candidate_it = candidate.begin();
  for (auto root_it = root.begin(); root_it != root.end(); ++root_it, ++candidate_it) {
    if (candidate_it == candidate.end() || !same_path(*candidate_it, *root_it)) return false;
  }
  return true;
}

std::filesystem::path platform_shell() {
#ifdef _WIN32
  std::wstring buffer(MAX_PATH, L'\0');
  const auto count = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
  if (count == 0 || count >= buffer.size()) return L"cmd.exe";
  buffer.resize(count);
  return std::filesystem::path(buffer) / L"cmd.exe";
#else
  return "/bin/sh";
#endif
}

audit::audit_event_outcome outcome_for(process_termination_reason reason) {
  switch (reason) {
    case process_termination_reason::exited: return audit::audit_event_outcome::completed;
    case process_termination_reason::timed_out: return audit::audit_event_outcome::timed_out;
    case process_termination_reason::cancelled: return audit::audit_event_outcome::cancelled;
    case process_termination_reason::policy_denied:
    case process_termination_reason::approval_denied:
      return audit::audit_event_outcome::denied;
    default: return audit::audit_event_outcome::failed;
  }
}

std::size_t saturated_add(std::size_t left, std::size_t right) {
  if (right > (std::numeric_limits<std::size_t>::max)() - left) {
    return (std::numeric_limits<std::size_t>::max)();
  }
  return left + right;
}

bool valid_environment_entry(
  std::string_view key,
  std::string_view value = {}) {
  return !key.empty() && key.find('=') == std::string_view::npos &&
         key.find('\0') == std::string_view::npos &&
         value.find('\0') == std::string_view::npos;
}

bool same_environment_key(
  std::string_view left,
  std::string_view right) {
#ifdef _WIN32
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(), [](char l, char r) {
           return std::tolower(static_cast<unsigned char>(l)) ==
                  std::tolower(static_cast<unsigned char>(r));
         });
#else
  return left == right;
#endif
}

bool environment_override_allowed(
  const std::set<std::string>& allowed,
  std::string_view key) {
  return std::any_of(allowed.begin(), allowed.end(), [&](const auto& candidate) {
    return same_environment_key(candidate, key);
  });
}

void set_environment_value(
  std::map<std::string, std::string>& environment,
  std::string key,
  std::string value) {
  const auto existing = std::find_if(
    environment.begin(), environment.end(), [&](const auto& entry) {
      return same_environment_key(entry.first, key);
    });
  if (existing != environment.end()) existing->second = std::move(value);
  else environment.emplace(std::move(key), std::move(value));
}

void erase_reserved_metadata(std::map<std::string, std::string>& metadata) {
  static constexpr std::array<std::string_view, 7> reserved {
    "operation_id",
    "shell",
    "executable",
    "launch_error_code",
    "launch_pipe_error_code",
    "wait_error_code",
    "signal",
  };
  for (const auto key : reserved) metadata.erase(std::string(key));
}

} // namespace

process_runtime::process_runtime(
  std::unique_ptr<process_backend> backend,
  process_policy policy,
  audit::audit_sink* audit,
  approval::approval_service* approvals)
    : backend_(std::move(backend)), policy_(std::move(policy)),
      audit_(audit), approvals_(approvals) {
  if (!backend_) throw std::invalid_argument("process_runtime requires a backend");
  if (policy_.working_directory_root.empty()) {
    throw std::invalid_argument("process_policy.working_directory_root is required");
  }
  std::error_code error;
  policy_.working_directory_root =
    std::filesystem::weakly_canonical(policy_.working_directory_root, error);
  if (error || !std::filesystem::is_directory(policy_.working_directory_root, error)) {
    throw std::invalid_argument("working_directory_root must be an existing directory");
  }
  if (policy_.default_workdir.empty()) policy_.default_workdir = ".";
  const auto default_workdir_input = policy_.default_workdir.is_absolute()
    ? policy_.default_workdir
    : policy_.working_directory_root / policy_.default_workdir;
  policy_.default_workdir =
    std::filesystem::weakly_canonical(default_workdir_input, error);
  if (error || !std::filesystem::is_directory(policy_.default_workdir, error) ||
      !path_within(policy_.default_workdir, policy_.working_directory_root)) {
    throw std::invalid_argument(
      "default_workdir must be an existing directory within working_directory_root");
  }
  if (policy_.shell_executable.empty()) policy_.shell_executable = platform_shell();
  if (policy_.max_limits.max_argument_count == 0 ||
      policy_.max_limits.max_argument_bytes == 0 ||
      policy_.max_limits.max_environment_count == 0 ||
      policy_.max_limits.max_environment_bytes == 0 ||
      policy_.max_limits.max_stdin_bytes == 0 ||
      policy_.max_limits.max_stdout_bytes == 0 ||
      policy_.max_limits.max_stderr_bytes == 0 ||
      policy_.max_limits.timeout.count() <= 0) {
    throw std::invalid_argument("process policy limits are invalid");
  }
  std::vector<std::string> environment_keys;
  environment_keys.reserve(policy_.base_environment.size());
  std::size_t environment_bytes = 0;
  for (const auto& [key, value] : policy_.base_environment) {
    if (!valid_environment_entry(key, value)) {
      throw std::invalid_argument("base_environment contains an invalid key or value");
    }
    if (std::any_of(
          environment_keys.begin(), environment_keys.end(),
          [&](const auto& existing) { return same_environment_key(existing, key); })) {
      throw std::invalid_argument(
        "base_environment contains duplicate platform-equivalent keys");
    }
    environment_keys.push_back(key);
    environment_bytes = saturated_add(
      environment_bytes, saturated_add(key.size(), saturated_add(value.size(), 2)));
  }
  if (policy_.base_environment.size() > policy_.max_limits.max_environment_count ||
      environment_bytes > policy_.max_limits.max_environment_bytes) {
    throw std::invalid_argument("base_environment exceeds the configured limits");
  }
  environment_keys.clear();
  for (const auto& key : policy_.allowed_environment_overrides) {
    if (!valid_environment_entry(key)) {
      throw std::invalid_argument(
        "allowed_environment_overrides contains an invalid key");
    }
    if (std::any_of(
          environment_keys.begin(), environment_keys.end(),
          [&](const auto& existing) { return same_environment_key(existing, key); })) {
      throw std::invalid_argument(
        "allowed_environment_overrides contains duplicate platform-equivalent keys");
    }
    environment_keys.push_back(key);
  }
  for (auto& executable : policy_.allowed_executables) {
    if (executable.empty()) {
      throw std::invalid_argument("allowed executable entries must not be empty");
    }
    if (executable.has_parent_path()) {
      if (!executable.is_absolute()) {
        throw std::invalid_argument(
          "allowed executable paths must be absolute or a bare filename");
      }
      executable = std::filesystem::weakly_canonical(executable, error);
      if (error || !std::filesystem::is_regular_file(executable, error)) {
        throw std::invalid_argument("allowed executable path is not a regular file");
      }
    }
    else if (executable == "." || executable == ".." ||
             executable.has_root_path()) {
      throw std::invalid_argument(
        "bare allowed executable names must name a file");
    }
  }
  for (auto& search_path : policy_.executable_search_paths) {
    search_path = std::filesystem::weakly_canonical(search_path, error);
    if (error || !search_path.is_absolute() ||
        !std::filesystem::is_directory(search_path, error)) {
      throw std::invalid_argument("executable search paths must be existing absolute directories");
    }
  }
  if (policy_.allow_shell) {
    policy_.shell_executable =
      std::filesystem::weakly_canonical(policy_.shell_executable, error);
    if (error || !policy_.shell_executable.is_absolute() ||
        !std::filesystem::is_regular_file(policy_.shell_executable, error)) {
      throw std::invalid_argument("shell_executable must be an existing absolute file");
    }
  }
}

process_result process_runtime::run(
  process_request request,
  std::stop_token stop_token) {
  return run_impl(std::move(request), false, stop_token);
}

process_result process_runtime::run_shell(
  shell_request request,
  std::stop_token stop_token) {
  process_request process {
    .executable = policy_.shell_executable,
#ifdef _WIN32
    .arguments = { "/D", "/S", "/C", request.command },
#else
    .arguments = { "-c", request.command },
#endif
    .stdin_text = std::move(request.stdin_text),
    .workdir = std::move(request.workdir),
    .environment = std::move(request.environment),
    .limits = request.limits,
    .metadata = std::move(request.metadata),
  };
  process.metadata["shell"] = "true";
  return run_impl(std::move(process), true, stop_token);
}

process_result process_runtime::run_impl(
  process_request request,
  bool shell,
  std::stop_token stop_token) {
  erase_reserved_metadata(request.metadata);
  const auto operation_id = "process-" + std::to_string(next_process_id_.fetch_add(1));
  const auto started = std::chrono::steady_clock::now();
  const auto denied = [&](std::string message, process_termination_reason reason = process_termination_reason::policy_denied) {
    process_result result {
      .termination_reason = reason,
      .error_message = std::move(message),
      .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started),
      .metadata = request.metadata,
    };
    result.metadata["operation_id"] = operation_id;
    result.metadata["shell"] = shell ? "true" : "false";
    audit::audit_event event {
      .module = "process",
      .name = "operation_finished",
      .id = operation_id,
      .subject_id = operation_id,
      .outcome = outcome_for(reason),
      .elapsed = result.elapsed,
      .attributes = result.metadata,
    };
    event.attributes["error"] = result.error_message;
    safely_publish(audit_, event);
    return result;
  };
  if (stop_token.stop_requested()) {
    return denied("process cancelled before launch", process_termination_reason::cancelled);
  }
  if (shell && !policy_.allow_shell) return denied("shell execution is disabled");
  if (shell && (request.arguments.empty() || request.arguments.back().empty())) {
    return denied("shell command must not be empty");
  }
  if (request.executable.empty()) return denied("executable must not be empty");

  request.limits.timeout = request.limits.timeout.count() <= 0
    ? policy_.max_limits.timeout
    : (std::min)(request.limits.timeout, policy_.max_limits.timeout);
  const auto normalize_limit = [](std::size_t requested, std::size_t maximum) {
    return requested == 0 ? maximum : (std::min)(requested, maximum);
  };
  request.limits.max_stdout_bytes = normalize_limit(
    request.limits.max_stdout_bytes, policy_.max_limits.max_stdout_bytes);
  request.limits.max_stderr_bytes = normalize_limit(
    request.limits.max_stderr_bytes, policy_.max_limits.max_stderr_bytes);
  request.limits.max_stdin_bytes = normalize_limit(
    request.limits.max_stdin_bytes, policy_.max_limits.max_stdin_bytes);
  request.limits.max_argument_bytes = normalize_limit(
    request.limits.max_argument_bytes, policy_.max_limits.max_argument_bytes);
  request.limits.max_argument_count = normalize_limit(
    request.limits.max_argument_count, policy_.max_limits.max_argument_count);
  request.limits.max_environment_bytes = normalize_limit(
    request.limits.max_environment_bytes, policy_.max_limits.max_environment_bytes);
  request.limits.max_environment_count = normalize_limit(
    request.limits.max_environment_count, policy_.max_limits.max_environment_count);

  if (request.arguments.size() > request.limits.max_argument_count) {
    return denied("argument count exceeds the configured maximum");
  }
  std::size_t argument_bytes = 0;
  for (const auto& argument : request.arguments) {
    if (argument.find('\0') != std::string::npos) return denied("arguments must not contain NUL bytes");
    argument_bytes = saturated_add(argument_bytes, argument.size());
  }
  if (argument_bytes > request.limits.max_argument_bytes) {
    return denied("arguments exceed the configured byte limit");
  }
  if (request.stdin_text.size() > request.limits.max_stdin_bytes) {
    return denied("stdin exceeds the configured byte limit");
  }

  std::error_code error;
  auto workdir_input = request.workdir.empty() ? policy_.default_workdir : request.workdir;
  auto workdir = std::filesystem::weakly_canonical(
    workdir_input.is_absolute() ? workdir_input
                                : policy_.working_directory_root / workdir_input,
    error);
  if (error || !std::filesystem::is_directory(workdir, error) ||
      !path_within(workdir, policy_.working_directory_root)) {
    return denied("working directory is invalid or outside working_directory_root");
  }

  std::filesystem::path executable;
  if (shell) {
    executable = std::filesystem::weakly_canonical(request.executable, error);
    if (error || !std::filesystem::is_regular_file(executable, error)) {
      return denied("configured shell executable is unavailable");
    }
  }
  else {
    const auto requested_bare = !request.executable.is_absolute() &&
                                !request.executable.has_parent_path();
    std::vector<std::filesystem::path> candidates;
    if (request.executable.is_absolute() || request.executable.has_parent_path()) {
      const auto candidate = request.executable.is_absolute()
        ? request.executable
        : policy_.working_directory_root / request.executable;
      candidates.push_back(std::filesystem::weakly_canonical(candidate, error));
      if (error) candidates.clear();
    }
    else {
      for (const auto& allowed : policy_.allowed_executables) {
        if (allowed.is_absolute() && same_path(allowed.filename(), request.executable.filename())) {
          candidates.push_back(std::filesystem::weakly_canonical(allowed, error));
          error.clear();
        }
      }
      const auto basename_allowed = std::any_of(
        policy_.allowed_executables.begin(), policy_.allowed_executables.end(),
        [&](const auto& allowed) {
          return !allowed.has_parent_path() && same_path(allowed, request.executable);
        });
      if (basename_allowed) {
        for (const auto& search_path : policy_.executable_search_paths) {
          candidates.push_back(std::filesystem::weakly_canonical(search_path / request.executable, error));
          error.clear();
        }
      }
    }
    for (const auto& candidate : candidates) {
      if (candidate.empty() || !std::filesystem::is_regular_file(candidate, error)) {
        error.clear();
        continue;
      }
      const auto allowed = std::any_of(
        policy_.allowed_executables.begin(), policy_.allowed_executables.end(),
        [&](const auto& configured) {
          if (!configured.has_parent_path()) {
            return requested_bare && same_path(configured, request.executable.filename());
          }
          std::error_code configured_error;
          const auto normalized = std::filesystem::weakly_canonical(configured, configured_error);
          return !configured_error && same_path(normalized, candidate);
        });
      if (allowed) { executable = candidate; break; }
    }
    if (executable.empty()) return denied("executable is not in the configured allowlist");
  }

  std::map<std::string, std::string> environment = policy_.base_environment;
  for (const auto& [key, value] : request.environment) {
    if (!valid_environment_entry(key, value)) {
      return denied("environment contains an invalid key or value");
    }
    if (!environment_override_allowed(policy_.allowed_environment_overrides, key)) {
      return denied("environment override is not allowed: " + key);
    }
    set_environment_value(environment, key, value);
  }
  std::size_t effective_environment_bytes = 0;
  for (const auto& [key, value] : environment) {
    effective_environment_bytes = saturated_add(
      effective_environment_bytes,
      saturated_add(key.size(), saturated_add(value.size(), 2)));
  }
  if (environment.size() > request.limits.max_environment_count) {
    return denied("environment entry count exceeds the configured maximum");
  }
  if (effective_environment_bytes > request.limits.max_environment_bytes) {
    return denied("environment exceeds the configured byte limit");
  }
  request.environment = std::move(environment);
  request.inherit_parent_environment = policy_.inherit_parent_environment;
  request.executable = executable;
  request.workdir = workdir;
  request.metadata["operation_id"] = operation_id;
  request.metadata["shell"] = shell ? "true" : "false";

  capability::capability_request capability_request {
    .name = shell ? capability::names::process_shell : capability::names::process_execute,
    .risk = shell ? capability::capability_risk_level::critical : capability::capability_risk_level::high,
    .summary = shell ? "Execute an approved shell command" : "Execute an approved process",
    .resources = { executable.generic_string(), workdir.generic_string() },
    .tool_name = shell ? "run_shell" : "run_process",
    .metadata = request.metadata,
  };
  const auto approval_required = shell ? policy_.require_approval_for_shell
                                       : policy_.require_approval_for_process;
  if (approval_required) {
    if (!approvals_) return denied("approval required but no approval service is configured", process_termination_reason::approval_denied);
    approval::approval_decision decision;
    try {
      decision = approvals_->decide({
        .id = operation_id,
        .summary = capability_request.summary,
        .capabilities = { capability_request },
        .metadata = request.metadata,
      });
    }
    catch (const std::exception& exception) {
      return denied(std::string("approval service failed: ") + exception.what(), process_termination_reason::approval_denied);
    }
    catch (...) {
      return denied("approval service failed with an unknown exception", process_termination_reason::approval_denied);
    }
    if (decision.kind != approval::approval_decision_kind::approved) {
      return denied(decision.reason.empty() ? "approval denied" : decision.reason,
        process_termination_reason::approval_denied);
    }
  }

  audit::audit_event launch {
    .module = "process",
    .name = "process_started",
    .id = operation_id,
    .subject_id = operation_id,
    .outcome = audit::audit_event_outcome::started,
    .attributes = {
      { "executable", executable.generic_string() },
      { "workdir", workdir.generic_string() },
      { "argument_count", std::to_string(request.arguments.size()) },
      { "shell", shell ? "true" : "false" },
    },
  };
  safely_publish(audit_, launch);

  process_result result;
  try { result = backend_->run(request, stop_token); }
  catch (const std::exception& exception) {
    result = { .termination_reason = process_termination_reason::backend_error, .error_message = exception.what() };
  }
  catch (...) {
    result = { .termination_reason = process_termination_reason::backend_error, .error_message = "process backend threw an unknown exception" };
  }
  if (result.elapsed.count() == 0) {
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
  }
  result.metadata["operation_id"] = operation_id;
  result.metadata["executable"] = executable.generic_string();
  result.metadata["shell"] = shell ? "true" : "false";
  audit::audit_event finished {
    .module = "process",
    .name = "process_finished",
    .id = operation_id,
    .subject_id = operation_id,
    .outcome = outcome_for(result.termination_reason),
    .elapsed = result.elapsed,
    .attributes = {
      { "termination_reason", to_string(result.termination_reason) },
      { "stdout_bytes", std::to_string(result.stdout_text.size()) },
      { "stderr_bytes", std::to_string(result.stderr_text.size()) },
      { "stdout_truncated", result.stdout_truncated ? "true" : "false" },
      { "stderr_truncated", result.stderr_truncated ? "true" : "false" },
    },
  };
  if (result.exit_code) finished.attributes["exit_code"] = std::to_string(*result.exit_code);
  if (!result.error_message.empty()) finished.attributes["error"] = result.error_message;
  safely_publish(audit_, finished);
  return result;
}

const process_policy& process_runtime::policy() const noexcept { return policy_; }
const process_backend* process_runtime::backend() const noexcept { return backend_.get(); }

void process_runtime::audit_tool_rejection(
  const std::string& tool_name,
  const std::string& reason,
  const std::map<std::string, std::string>& attributes) {
  audit::audit_event event {
    .module = "process",
    .name = "tool_rejected",
    .id = "process-" + std::to_string(next_process_id_.fetch_add(1)),
    .outcome = audit::audit_event_outcome::denied,
    .attributes = attributes,
  };
  event.subject_id = event.id;
  event.attributes["tool_name"] = tool_name;
  event.attributes["reason"] = reason;
  safely_publish(audit_, event);
}

} // namespace wuwe::agent::process
