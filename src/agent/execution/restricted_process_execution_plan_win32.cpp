#include "restricted_process_execution_plan_win32.hpp"

#ifdef _WIN32

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include "restricted_process_acl_win32.hpp"
#include "restricted_process_path_win32.hpp"

namespace wuwe::agent::execution::detail {
namespace {

std::atomic<unsigned long long> plan_counter {};
std::mutex restricted_acl_execution_mutex;

restricted_execution_plan_result make_plan_result(
  restricted_execution_plan_status status, std::string detail = {}) {
  return {
    .status = status,
    .detail = std::move(detail),
  };
}

restricted_execution_plan_result make_failed_plan_result_with_cleanup(
  restricted_execution_plan_status status, std::string detail,
  restricted_appcontainer_profile& profile, restricted_acl_lease* acl_lease = nullptr,
  restricted_request_workspace* workspace = nullptr) {
  auto result = make_plan_result(status, std::move(detail));
  if (acl_lease != nullptr) {
    restricted_acl_grant_result cleanup;
    do {
      ++result.acl_cleanup_attempts;
      cleanup = acl_lease->restore();
    } while (cleanup.status != restricted_acl_grant_status::ok && result.acl_cleanup_attempts < 3);
    result.acl_cleanup = std::move(cleanup);
  }
  if (workspace != nullptr) {
    workspace->close_security_locks();
  }
  restricted_appcontainer_profile_cleanup_result cleanup;
  do {
    ++result.profile_cleanup_attempts;
    cleanup = profile.cleanup();
  } while (cleanup.status != restricted_appcontainer_profile_cleanup_status::ok &&
           result.profile_cleanup_attempts < 3);
  result.profile_cleanup = std::move(cleanup);
  return result;
}

std::string next_profile_name() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto counter = plan_counter.fetch_add(1, std::memory_order_relaxed);
  return "wuwe-restricted-exec-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(now) + "-" + std::to_string(counter);
}

std::optional<std::wstring> widen_utf8(std::string_view text) {
  if (text.empty()) {
    return std::wstring {};
  }
  const auto required = MultiByteToWideChar(
    CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (required <= 0) {
    return std::nullopt;
  }
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  if (MultiByteToWideChar(CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        required) != required) {
    return std::nullopt;
  }
  return result;
}

std::string narrow_ascii(std::wstring_view text) {
  std::string result;
  result.reserve(text.size());
  for (const auto ch : text) {
    result.push_back(static_cast<char>(ch));
  }
  return result;
}

bool equal_environment_name(std::wstring_view left, std::wstring_view right) {
  return CompareStringOrdinal(left.data(),
           static_cast<int>(left.size()),
           right.data(),
           static_cast<int>(right.size()),
           TRUE) == CSTR_EQUAL;
}

std::optional<std::string> validate_request_environment(
  const std::map<std::string, std::string>& environment) {
  std::vector<std::wstring> names;
  names.reserve(environment.size());
  for (const auto& [name, value] : environment) {
    if (name.empty() || name.find('=') != std::string::npos ||
        name.find('\0') != std::string::npos || value.find('\0') != std::string::npos) {
      return "environment names must be non-empty, must not contain '=', and names and values "
             "must not contain NUL bytes";
    }
    auto wide_name = widen_utf8(name);
    if (!wide_name || !widen_utf8(value)) {
      return "environment contains text that is not valid UTF-8";
    }
    if (std::any_of(names.begin(), names.end(), [&](const auto& existing) {
          return equal_environment_name(existing, *wide_name);
        })) {
      return "environment contains case-insensitive duplicate names";
    }
    names.push_back(std::move(*wide_name));
  }
  return std::nullopt;
}

void set_environment_value(
  std::map<std::wstring, std::wstring>& environment, std::wstring name, std::wstring value) {
  for (auto it = environment.begin(); it != environment.end();) {
    if (equal_environment_name(it->first, name)) {
      it = environment.erase(it);
    }
    else {
      ++it;
    }
  }
  environment.emplace(std::move(name), std::move(value));
}

void inherit_current_environment(std::map<std::wstring, std::wstring>& environment) {
  const auto raw = GetEnvironmentStringsW();
  if (raw == nullptr) {
    return;
  }
  for (const wchar_t* cursor = raw; *cursor != L'\0';) {
    const std::wstring_view entry(cursor);
    cursor += entry.size() + 1;
    const auto separator = entry.find(L'=', entry.starts_with(L'=') ? 1 : 0);
    if (separator == std::wstring_view::npos || separator == 0) {
      continue;
    }
    set_environment_value(environment,
      std::wstring(entry.substr(0, separator)),
      std::wstring(entry.substr(separator + 1)));
  }
  FreeEnvironmentStringsW(raw);
}

void set_current_environment_value_if_present(
  std::map<std::wstring, std::wstring>& environment, const wchar_t* name) {
  std::wstring value(32767, L'\0');
  const auto written =
    GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
  if (written > 0 && written < value.size()) {
    value.resize(written);
    set_environment_value(environment, name, std::move(value));
  }
}

std::optional<std::map<std::wstring, std::wstring>> make_environment(
  const windows_restricted_process_sandbox_plan& sandbox_plan,
  const restricted_appcontainer_profile& profile, const execution_request& request) {
  const auto& config = sandbox_plan.runtime_config();
  std::map<std::wstring, std::wstring> result;
  if (config.inherit_parent_environment) {
    inherit_current_environment(result);
  }
  for (const auto& [name, value] : config.base_environment) {
    auto wide_name = widen_utf8(name);
    auto wide_value = widen_utf8(value);
    if (!wide_name || !wide_value) {
      return std::nullopt;
    }
    set_environment_value(result, std::move(*wide_name), std::move(*wide_value));
  }
  for (const auto& [name, value] : request.env) {
    auto wide_name = widen_utf8(name);
    auto wide_value = widen_utf8(value);
    if (!wide_name || !wide_value) {
      return std::nullopt;
    }
    set_environment_value(result, std::move(*wide_name), std::move(*wide_value));
  }
  set_current_environment_value_if_present(result, L"SystemRoot");
  const auto profile_home = profile.storage_path().wstring();
  for (const auto* name : { L"USERPROFILE", L"LOCALAPPDATA", L"APPDATA" }) {
    set_environment_value(result, name, profile_home);
  }
  const auto temp = (profile.storage_path() / "Temp").wstring();
  set_environment_value(result, L"TEMP", temp);
  set_environment_value(result, L"TMP", temp);
  return result;
}

std::filesystem::path workspace_root_for(
  const restricted_process_backend_config& config, const restricted_appcontainer_profile& profile) {
  if (!config.fallback_workdir.empty()) {
    return config.fallback_workdir;
  }
  return profile.storage_path() / "requests";
}

std::optional<std::filesystem::path> requested_working_directory(
  const windows_restricted_process_sandbox_plan& sandbox_plan, const execution_request& request) {
  if (request.workdir.empty()) {
    return std::nullopt;
  }
  std::error_code error;
  auto workdir = request.workdir.is_absolute() ? request.workdir
                                               : std::filesystem::absolute(request.workdir, error);
  if (error) {
    return std::filesystem::path {};
  }
  workdir = workdir.lexically_normal();
  if (sandbox_plan.access_for(workdir) == restricted_path_access::denied ||
      !std::filesystem::is_directory(workdir, error) || error) {
    return std::filesystem::path {};
  }
  return workdir;
}

std::filesystem::path runtime_root_for(
  const restricted_process_backend_config& config, const restricted_appcontainer_profile& profile) {
  if (!config.runtime_staging_root.empty()) {
    return config.runtime_staging_root / profile.name() / "runtime";
  }
  return profile.storage_path() / "runtime";
}

class external_runtime_cleanup_guard {
public:
  external_runtime_cleanup_guard(
    const restricted_process_backend_config& config, const restricted_appcontainer_profile& profile)
      : active_(config.cleanup_runtime_staging && !config.runtime_staging_root.empty()) {
    if (active_) {
      root_ = config.runtime_staging_root / profile.name();
    }
  }

  ~external_runtime_cleanup_guard() {
    if (active_ && !root_.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(root_, ignored);
    }
  }

  external_runtime_cleanup_guard(const external_runtime_cleanup_guard&) = delete;
  external_runtime_cleanup_guard& operator=(const external_runtime_cleanup_guard&) = delete;

  void release() noexcept {
    active_ = false;
  }

private:
  std::filesystem::path root_;
  bool active_ { false };
};

restricted_execution_plan_result grant_tree_or_fail(const std::filesystem::path& path, PSID sid,
  DWORD directory_access, DWORD file_access, restricted_acl_lease& lease,
  restricted_path_access access, std::string_view label) {
  const auto request = restricted_acl_grant_request {
    .path = path,
    .sid = sid,
    .directory_access = directory_access,
    .file_access = file_access,
    .lease = &lease,
  };
  restricted_acl_grant_result grant;
  switch (access) {
    case restricted_path_access::readable:
    case restricted_path_access::writable:
      grant = grant_restricted_tree_access(request);
      break;
    case restricted_path_access::protected_read_only:
      grant = protect_restricted_tree_read_only(request);
      break;
    case restricted_path_access::denied:
      grant = deny_restricted_tree_access(request);
      break;
  }
  if (grant.status != restricted_acl_grant_status::ok) {
    return make_plan_result(restricted_execution_plan_status::acl_grant_failed,
      std::string(label) + ": " + to_string(grant.status) + " " + grant.detail);
  }
  return {};
}

} // namespace

const char* to_string(restricted_execution_plan_status status) noexcept {
  switch (status) {
    case restricted_execution_plan_status::ok:
      return "ok";
    case restricted_execution_plan_status::sandbox_compile_failed:
      return "sandbox_compile_failed";
    case restricted_execution_plan_status::unsupported_language:
      return "unsupported_language";
    case restricted_execution_plan_status::profile_failed:
      return "profile_failed";
    case restricted_execution_plan_status::runtime_staging_failed:
      return "runtime_staging_failed";
    case restricted_execution_plan_status::workspace_failed:
      return "workspace_failed";
    case restricted_execution_plan_status::working_directory_denied:
      return "working_directory_denied";
    case restricted_execution_plan_status::invalid_environment:
      return "invalid_environment";
    case restricted_execution_plan_status::invalid_limits:
      return "invalid_limits";
    case restricted_execution_plan_status::acl_grant_failed:
      return "acl_grant_failed";
  }
  return "unknown";
}

restricted_execution_plan_result prepare_restricted_execution_plan(
  const windows_restricted_process_sandbox_plan& sandbox_plan, const execution_request& request) {
  if (request.language != execution_language::python) {
    return make_plan_result(restricted_execution_plan_status::unsupported_language);
  }
  if (const auto environment_error = validate_request_environment(request.env)) {
    return make_plan_result(
      restricted_execution_plan_status::invalid_environment, *environment_error);
  }
  if (request.limits.timeout < std::chrono::milliseconds::zero() ||
      request.limits.max_cpu_time < std::chrono::milliseconds::zero()) {
    return make_plan_result(restricted_execution_plan_status::invalid_limits,
      "timeout and CPU time limits must not be negative");
  }
  const auto& config = sandbox_plan.runtime_config();

  auto profile_result = create_restricted_appcontainer_profile({
    .name = next_profile_name(),
    .display_name = L"Wuwe Restricted Execution",
    .description = L"Wuwe restricted execution AppContainer profile",
  });
  if (profile_result.status != restricted_appcontainer_profile_status::ok ||
      !profile_result.profile.has_value()) {
    return make_plan_result(restricted_execution_plan_status::profile_failed,
      to_string(profile_result.status) + std::string(" ") + profile_result.detail);
  }

  auto profile = std::move(*profile_result.profile);
  restricted_windows_locked_path profile_temp_lock;
  const auto profile_temp_result =
    create_restricted_windows_directories(profile.storage_path() / "Temp", profile_temp_lock);
  if (!profile_temp_result) {
    return make_failed_plan_result_with_cleanup(restricted_execution_plan_status::workspace_failed,
      "profile_temp: " + std::string(to_string(profile_temp_result.status)) + " " +
        profile_temp_result.detail,
      profile);
  }
  profile_temp_lock.reset();
  const auto runtime_root = runtime_root_for(config, profile);
  external_runtime_cleanup_guard staging_cleanup(config, profile);
  auto runtime_staging = stage_minimal_python_runtime_for_restricted_process({
    .source_python = config.python_interpreter,
    .destination_home = runtime_root,
    .replace_existing = true,
  });
  if (runtime_staging.status != restricted_python_runtime_staging_status::ok) {
    return make_failed_plan_result_with_cleanup(
      restricted_execution_plan_status::runtime_staging_failed,
      to_string(runtime_staging.status) + std::string(" ") + runtime_staging.detail,
      profile);
  }

  auto workspace_result = create_restricted_request_workspace({
    .root = workspace_root_for(config, profile),
    .script_text = request.code,
    .script_filename = "snippet.py",
    .cleanup_on_destroy = config.cleanup_runtime_staging,
  });
  if (workspace_result.status != restricted_request_workspace_status::ok ||
      !workspace_result.workspace.has_value()) {
    return make_failed_plan_result_with_cleanup(restricted_execution_plan_status::workspace_failed,
      to_string(workspace_result.status) + std::string(" ") + workspace_result.detail,
      profile);
  }

  auto workspace = std::move(*workspace_result.workspace);
  const auto requested_workdir = requested_working_directory(sandbox_plan, request);
  if (!request.workdir.empty() && (!requested_workdir || requested_workdir->empty())) {
    return make_failed_plan_result_with_cleanup(
      restricted_execution_plan_status::working_directory_denied,
      request.workdir.string(),
      profile,
      nullptr,
      &workspace);
  }
  const auto working_directory = requested_workdir.value_or(workspace.root());
  restricted_acl_lease acl_lease;
  constexpr DWORD read_execute = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
  constexpr DWORD writable_directory =
    FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
  constexpr DWORD writable_file = FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE;
  if (auto grant = grant_tree_or_fail(runtime_root,
        profile.sid(),
        read_execute,
        read_execute,
        acl_lease,
        restricted_path_access::readable,
        "runtime");
      grant.status != restricted_execution_plan_status::ok) {
    return make_failed_plan_result_with_cleanup(
      grant.status, grant.detail, profile, &acl_lease, &workspace);
  }
  if (auto grant = grant_tree_or_fail(workspace.root(),
        profile.sid(),
        read_execute,
        read_execute,
        acl_lease,
        restricted_path_access::readable,
        "workspace");
      grant.status != restricted_execution_plan_status::ok) {
    return make_failed_plan_result_with_cleanup(
      grant.status, grant.detail, profile, &acl_lease, &workspace);
  }
  for (const auto& root : sandbox_plan.policy().filesystem.readable_roots) {
    if (auto grant = grant_tree_or_fail(root,
          profile.sid(),
          read_execute,
          read_execute,
          acl_lease,
          restricted_path_access::readable,
          "readable_root");
        grant.status != restricted_execution_plan_status::ok) {
      return make_failed_plan_result_with_cleanup(
        grant.status, grant.detail, profile, &acl_lease, &workspace);
    }
  }
  for (const auto& root : sandbox_plan.policy().filesystem.writable_roots) {
    if (auto grant = grant_tree_or_fail(root,
          profile.sid(),
          writable_directory,
          writable_file,
          acl_lease,
          restricted_path_access::writable,
          "writable_root");
        grant.status != restricted_execution_plan_status::ok) {
      return make_failed_plan_result_with_cleanup(
        grant.status, grant.detail, profile, &acl_lease, &workspace);
    }
  }
  for (const auto& root : sandbox_plan.policy().filesystem.protected_read_only_paths) {
    if (auto grant = grant_tree_or_fail(root,
          profile.sid(),
          read_execute,
          read_execute,
          acl_lease,
          restricted_path_access::protected_read_only,
          "protected_read_only_path");
        grant.status != restricted_execution_plan_status::ok) {
      return make_failed_plan_result_with_cleanup(
        grant.status, grant.detail, profile, &acl_lease, &workspace);
    }
  }
  for (const auto& root : sandbox_plan.policy().filesystem.denied_paths) {
    if (auto grant = grant_tree_or_fail(
          root, profile.sid(), 0, 0, acl_lease, restricted_path_access::denied, "denied_path");
        grant.status != restricted_execution_plan_status::ok) {
      return make_failed_plan_result_with_cleanup(
        grant.status, grant.detail, profile, &acl_lease, &workspace);
    }
  }
  auto environment = make_environment(sandbox_plan, profile, request);
  if (!environment) {
    return make_failed_plan_result_with_cleanup(
      restricted_execution_plan_status::invalid_environment,
      "environment contains text that is not valid UTF-8",
      profile,
      &acl_lease,
      &workspace);
  }

  restricted_appcontainer_launch_request launch_request {
    .executable = runtime_staging.python_executable,
    .appcontainer_sid = profile.sid(),
    .arguments = {
      L"-I",
      L"-S",
      workspace.script_path().wstring(),
    },
    .working_directory = working_directory,
    .stdin_text = request.stdin_text,
    .timeout = request.limits.timeout,
    .max_stdout_bytes = request.limits.max_stdout_bytes,
    .max_stderr_bytes = request.limits.max_stderr_bytes,
    .use_job_object = config.use_job_object,
    .max_process_count = sandbox_plan.constrain_process_count(request.limits.max_process_count),
    .max_memory_bytes = sandbox_plan.constrain_memory_bytes(request.limits.max_memory_bytes),
    .max_cpu_time = sandbox_plan.constrain_cpu_time(request.limits.max_cpu_time),
    .environment = std::move(*environment),
  };
  const auto python_executable = runtime_staging.python_executable;

  restricted_execution_plan plan {
    .profile = std::move(profile),
    .acl_lease = std::move(acl_lease),
    .workspace = std::move(workspace),
    .runtime_staging = std::move(runtime_staging),
    .launch_request = std::move(launch_request),
    .runtime_root = runtime_root,
    .python_executable = python_executable,
  };
  staging_cleanup.release();

  return {
    .status = restricted_execution_plan_status::ok,
    .plan = std::move(plan),
  };
}

restricted_execution_plan_result prepare_restricted_execution_plan(
  const restricted_process_backend_config& config, const execution_request& request) {
  auto compiler = make_restricted_process_sandbox_backend(config);
  auto compiled = compiler->compile(restricted_process_sandbox_policy(config));
  if (!compiled) {
    return make_plan_result(
      restricted_execution_plan_status::sandbox_compile_failed, compiled.message);
  }
  auto native = as_windows_restricted_process_sandbox_plan(compiled.plan);
  if (!native) {
    return make_plan_result(restricted_execution_plan_status::sandbox_compile_failed,
      "compiler returned a foreign sandbox plan");
  }
  return prepare_restricted_execution_plan(*native, request);
}

execution_termination_reason termination_for_launch(
  const restricted_appcontainer_launch_result& launch) {
  if (launch.capture.timed_out) {
    return execution_termination_reason::timeout;
  }
  if (launch.capture.cancelled) {
    return execution_termination_reason::cancelled;
  }
  if (launch.status != restricted_appcontainer_launch_status::ok) {
    return execution_termination_reason::launch_failed;
  }
  return execution_termination_reason::exited;
}

void add_restricted_metadata(execution_result& result,
  const windows_restricted_process_sandbox_plan& sandbox_plan, const execution_request& request) {
  const auto& config = sandbox_plan.runtime_config();
  const auto& enforcement = sandbox_plan.enforcement();
  result.metadata["backend_name"] = "restricted_process";
  result.metadata["isolation_level"] = "restricted_process";
  result.metadata["backend_available"] = "true";
  result.metadata["backend_stage"] = "native_sandbox_plan";
  result.metadata["sandbox_policy_name"] = sandbox_plan.policy().name;
  result.metadata["sandbox_plan_id"] = std::to_string(sandbox_plan.plan_id());
  result.metadata["sandbox_plan_format_version"] = std::to_string(sandbox_plan.format_version());
  result.metadata["filesystem_platform_defaults"] =
    sandbox_plan.metadata().at("filesystem_platform_defaults");
  result.metadata["python_interpreter"] = config.python_interpreter.string();
  result.metadata["deny_network"] = config.deny_network ? "true" : "false";
  result.metadata["use_job_object"] = config.use_job_object ? "true" : "false";
  result.metadata["inherit_parent_environment"] =
    config.inherit_parent_environment ? "true" : "false";
  result.metadata["cleanup_runtime_staging"] = config.cleanup_runtime_staging ? "true" : "false";
  result.metadata["readable_roots_count"] =
    std::to_string(sandbox_plan.policy().filesystem.readable_roots.size());
  result.metadata["writable_roots_count"] =
    std::to_string(sandbox_plan.policy().filesystem.writable_roots.size());
  result.metadata["protected_read_only_paths_count"] =
    std::to_string(sandbox_plan.policy().filesystem.protected_read_only_paths.size());
  result.metadata["denied_paths_count"] =
    std::to_string(sandbox_plan.policy().filesystem.denied_paths.size());
  result.metadata["timeout_ms"] = std::to_string(request.limits.timeout.count());
  result.metadata["max_stdout_bytes"] = std::to_string(request.limits.max_stdout_bytes);
  result.metadata["max_stderr_bytes"] = std::to_string(request.limits.max_stderr_bytes);
  result.metadata["shell_execution_enforcement"] = sandbox::to_string(enforcement.shell_execution);
  result.metadata["timeout_enforcement"] = sandbox::to_string(enforcement.timeout);
  result.metadata["cancellation_enforcement"] = sandbox::to_string(enforcement.cancellation);
  result.metadata["stdout_limit_enforcement"] = sandbox::to_string(enforcement.stdout_limit);
  result.metadata["stderr_limit_enforcement"] = sandbox::to_string(enforcement.stderr_limit);
  result.metadata["environment_allowlist_enforcement"] =
    sandbox::to_string(enforcement.environment_allowlist);
  result.metadata["working_directory_enforcement"] =
    sandbox::to_string(enforcement.working_directory);
  result.metadata["process_count_limit_enforcement"] =
    sandbox::to_string(enforcement.process_count_limit);
  result.metadata["cpu_time_limit_enforcement"] = sandbox::to_string(enforcement.cpu_time_limit);
  result.metadata["memory_limit_enforcement"] = sandbox::to_string(enforcement.memory_limit);
  result.metadata["process_tree_cleanup_enforcement"] =
    sandbox::to_string(enforcement.process_tree_cleanup);
  result.metadata["file_read_deny_enforcement"] =
    sandbox::to_string(enforcement.filesystem_read_deny);
  result.metadata["file_write_deny_enforcement"] =
    sandbox::to_string(enforcement.filesystem_write_deny);
  result.metadata["network_deny_enforcement"] = sandbox::to_string(enforcement.network_deny);
  result.metadata["network_filter_enforcement"] = sandbox::to_string(enforcement.network_filter);
  result.metadata["max_process_count"] =
    std::to_string(sandbox_plan.constrain_process_count(request.limits.max_process_count));
  result.metadata["max_memory_bytes"] =
    std::to_string(sandbox_plan.constrain_memory_bytes(request.limits.max_memory_bytes));
  result.metadata["max_cpu_time_ms"] =
    std::to_string(sandbox_plan.constrain_cpu_time(request.limits.max_cpu_time).count());
}

void add_plan_metadata(execution_result& result, const restricted_execution_plan& plan) {
  result.metadata["appcontainer_profile"] = narrow_ascii(plan.profile.name());
  result.metadata["workspace_root"] = plan.workspace.root().string();
  result.metadata["script_path"] = plan.workspace.script_path().string();
  result.metadata["runtime_root"] = plan.runtime_root.string();
  result.metadata["python_executable"] = plan.python_executable.string();
  result.metadata["runtime_staging_files"] =
    std::to_string(plan.runtime_staging.copied_files.size());
}

void cleanup_external_runtime_staging(
  const restricted_process_backend_config& config, const restricted_execution_plan& plan) {
  if (!config.cleanup_runtime_staging || config.runtime_staging_root.empty()) {
    return;
  }

  const auto cleanup_root = config.runtime_staging_root / plan.profile.name();
  std::error_code ignored;
  std::filesystem::remove_all(cleanup_root, ignored);
}

execution_result run_restricted_execution_plan(
  std::shared_ptr<const windows_restricted_process_sandbox_plan> sandbox_plan,
  const execution_request& request, std::stop_token stop_token) {
  const auto started = std::chrono::steady_clock::now();
  execution_result result;
  if (!sandbox_plan) {
    result.termination_reason = execution_termination_reason::backend_error;
    result.error_message = "restricted execution requires a native sandbox plan";
    result.metadata["error_code"] = "restricted_plan_missing";
    return result;
  }
  const auto& config = sandbox_plan->runtime_config();
  add_restricted_metadata(result, *sandbox_plan, request);
  result.metadata["acl_concurrency_mode"] = "process_wide_serialized";
  std::unique_lock acl_execution_lock(restricted_acl_execution_mutex);

  auto plan_result = prepare_restricted_execution_plan(*sandbox_plan, request);
  result.metadata["restricted_plan_status"] = to_string(plan_result.status);
  if (plan_result.status != restricted_execution_plan_status::ok || !plan_result.plan.has_value()) {
    if (plan_result.status == restricted_execution_plan_status::working_directory_denied ||
        plan_result.status == restricted_execution_plan_status::invalid_environment ||
        plan_result.status == restricted_execution_plan_status::invalid_limits) {
      result.termination_reason = execution_termination_reason::policy_denied;
    }
    else if (plan_result.status == restricted_execution_plan_status::unsupported_language ||
             plan_result.status == restricted_execution_plan_status::sandbox_compile_failed) {
      result.termination_reason = execution_termination_reason::backend_error;
    }
    else {
      result.termination_reason = execution_termination_reason::launch_failed;
    }
    result.error_message = std::string("restricted execution plan failed: ") +
                           to_string(plan_result.status) + " " + plan_result.detail;
    result.metadata["error_code"] = std::string("restricted_plan_") + to_string(plan_result.status);
    if (plan_result.acl_cleanup.has_value()) {
      result.metadata["acl_lease_restore_status"] = to_string(plan_result.acl_cleanup->status);
      result.metadata["acl_lease_restore_attempts"] =
        std::to_string(plan_result.acl_cleanup_attempts);
      if (plan_result.acl_cleanup->status != restricted_acl_grant_status::ok) {
        result.termination_reason = execution_termination_reason::backend_error;
        result.error_message +=
          "; ACL cleanup failed: " + std::string(to_string(plan_result.acl_cleanup->status)) + " " +
          plan_result.acl_cleanup->detail;
        result.metadata["error_code"] = "restricted_acl_lease_restore_failed";
        result.metadata["acl_lease_restore_win32_error"] =
          std::to_string(plan_result.acl_cleanup->win32_error);
      }
    }
    if (plan_result.profile_cleanup.has_value()) {
      result.metadata["appcontainer_profile_cleanup_status"] =
        to_string(plan_result.profile_cleanup->status);
      result.metadata["appcontainer_profile_cleanup_attempts"] =
        std::to_string(plan_result.profile_cleanup_attempts);
      if (plan_result.profile_cleanup->status !=
          restricted_appcontainer_profile_cleanup_status::ok) {
        result.termination_reason = execution_termination_reason::backend_error;
        result.error_message += "; AppContainer profile cleanup failed: " +
                                std::string(to_string(plan_result.profile_cleanup->status)) + " " +
                                plan_result.profile_cleanup->detail;
        result.metadata["error_code"] = "restricted_appcontainer_profile_cleanup_failed";
        result.metadata["appcontainer_profile_cleanup_hresult"] =
          std::to_string(static_cast<long long>(plan_result.profile_cleanup->hresult));
        result.metadata["appcontainer_profile_cleanup_win32_error"] =
          std::to_string(plan_result.profile_cleanup->win32_error);
      }
    }
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
    return result;
  }

  auto& plan = *plan_result.plan;
  add_plan_metadata(result, plan);
  plan.launch_request.stop_token = stop_token;
  auto launch = launch_restricted_appcontainer_process(std::move(plan.launch_request));

  if (launch.capture.exit_code.has_value()) {
    result.exit_code = static_cast<int>(*launch.capture.exit_code);
  }
  result.timed_out = launch.capture.timed_out;
  result.cancelled = launch.capture.cancelled;
  result.stdout_truncated = launch.capture.stdout_truncated;
  result.stderr_truncated = launch.capture.stderr_truncated;
  result.stdout_text = std::move(launch.capture.stdout_text);
  result.stderr_text = std::move(launch.capture.stderr_text);
  result.termination_reason = termination_for_launch(launch);
  result.metadata["restricted_launch_status"] = to_string(launch.status);
  result.metadata["restricted_launch_win32_error"] = std::to_string(launch.win32_error);
  if (!launch.detail.empty()) {
    result.metadata["restricted_launch_detail"] = launch.detail;
  }
  if (launch.status != restricted_appcontainer_launch_status::ok) {
    result.error_message = std::string("restricted launch failed: ") + to_string(launch.status);
    result.metadata["error_code"] = std::string("restricted_launch_") + to_string(launch.status);
  }
  else if (result.timed_out) {
    result.error_message = "restricted execution timed out";
    result.metadata["error_code"] = "restricted_execution_timeout";
  }
  else if (result.cancelled) {
    result.error_message = "restricted execution cancelled";
    result.metadata["error_code"] = "restricted_execution_cancelled";
  }

  restricted_acl_grant_result acl_restore;
  int acl_restore_attempts = 0;
  do {
    ++acl_restore_attempts;
    acl_restore = plan.acl_lease.restore();
  } while (acl_restore.status != restricted_acl_grant_status::ok && acl_restore_attempts < 3);
  result.metadata["acl_lease_restore_status"] = to_string(acl_restore.status);
  result.metadata["acl_lease_restore_attempts"] = std::to_string(acl_restore_attempts);
  if (acl_restore.status != restricted_acl_grant_status::ok) {
    result.termination_reason = execution_termination_reason::backend_error;
    result.error_message =
      "restricted ACL lease cleanup failed: " + std::string(to_string(acl_restore.status)) + " " +
      acl_restore.detail;
    result.metadata["error_code"] = "restricted_acl_lease_restore_failed";
    result.metadata["acl_lease_restore_win32_error"] = std::to_string(acl_restore.win32_error);
  }
  plan.workspace.close_security_locks();
  cleanup_external_runtime_staging(config, plan);

  restricted_appcontainer_profile_cleanup_result profile_cleanup;
  int profile_cleanup_attempts = 0;
  do {
    ++profile_cleanup_attempts;
    profile_cleanup = plan.profile.cleanup();
  } while (profile_cleanup.status != restricted_appcontainer_profile_cleanup_status::ok &&
           profile_cleanup_attempts < 3);
  result.metadata["appcontainer_profile_cleanup_status"] = to_string(profile_cleanup.status);
  result.metadata["appcontainer_profile_cleanup_attempts"] =
    std::to_string(profile_cleanup_attempts);
  if (profile_cleanup.status != restricted_appcontainer_profile_cleanup_status::ok) {
    result.termination_reason = execution_termination_reason::backend_error;
    const auto cleanup_error =
      "AppContainer profile cleanup failed: " + std::string(to_string(profile_cleanup.status)) +
      " " + profile_cleanup.detail;
    if (result.error_message.empty()) {
      result.error_message = cleanup_error;
      result.metadata["error_code"] = "restricted_appcontainer_profile_cleanup_failed";
    }
    else {
      result.error_message += "; " + cleanup_error;
    }
    result.metadata["appcontainer_profile_cleanup_hresult"] =
      std::to_string(static_cast<long long>(profile_cleanup.hresult));
    result.metadata["appcontainer_profile_cleanup_win32_error"] =
      std::to_string(profile_cleanup.win32_error);
  }
  result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - started);
  return result;
}

execution_result run_restricted_execution_plan(const restricted_process_backend_config& config,
  const execution_request& request, std::stop_token stop_token) {
  auto compiler = make_restricted_process_sandbox_backend(config);
  auto compiled = compiler->compile(restricted_process_sandbox_policy(config));
  if (!compiled) {
    execution_result result {
      .termination_reason = execution_termination_reason::backend_error,
      .error_message = "restricted sandbox compilation failed: " + compiled.message,
    };
    result.metadata["backend_name"] = "restricted_process";
    result.metadata["error_code"] = "restricted_sandbox_compile_failed";
    if (!compiled.blockers.empty()) {
      std::string blockers;
      for (const auto& blocker : compiled.blockers) {
        if (!blockers.empty()) {
          blockers += ",";
        }
        blockers += blocker;
      }
      result.metadata["sandbox_compile_blockers"] = std::move(blockers);
    }
    return result;
  }
  return run_restricted_execution_plan(
    as_windows_restricted_process_sandbox_plan(compiled.plan), request, stop_token);
}

} // namespace wuwe::agent::execution::detail

#endif // _WIN32
