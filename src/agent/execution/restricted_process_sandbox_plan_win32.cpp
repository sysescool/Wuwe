#include "restricted_process_sandbox_plan_win32.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <string_view>
#include <utility>

namespace wuwe::agent::execution::detail {
namespace {

std::atomic<std::uint64_t> next_plan_id { 1 };

bool equal_component_case_insensitive(std::wstring_view left, std::wstring_view right) noexcept {
  return CompareStringOrdinal(left.data(),
           static_cast<int>(left.size()),
           right.data(),
           static_cast<int>(right.size()),
           TRUE) == CSTR_EQUAL;
}

bool path_contains_case_insensitive(
  const std::filesystem::path& root, const std::filesystem::path& candidate) noexcept {
  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  for (; root_it != root.end(); ++root_it, ++candidate_it) {
    if (candidate_it == candidate.end() ||
        !equal_component_case_insensitive(root_it->wstring(), candidate_it->wstring())) {
      return false;
    }
  }
  return true;
}

template<typename Value>
Value constrain_nonzero(Value requested, const std::optional<Value>& policy_limit) noexcept {
  if (!policy_limit) {
    return requested;
  }
  if (requested == Value {}) {
    return *policy_limit;
  }
  return (std::min)(requested, *policy_limit);
}

std::filesystem::path resolve_python_executable(const std::filesystem::path& configured) {
  if (configured.empty()) {
    return {};
  }

  std::error_code error;
  if (configured.is_absolute() || configured.has_parent_path()) {
    auto absolute =
      configured.is_absolute() ? configured : std::filesystem::absolute(configured, error);
    if (!error && std::filesystem::is_regular_file(absolute, error) && !error) {
      return absolute.lexically_normal();
    }
    return {};
  }

  std::wstring buffer(32768, L'\0');
  const auto written = SearchPathW(nullptr,
    configured.wstring().c_str(),
    configured.has_extension() ? nullptr : L".exe",
    static_cast<DWORD>(buffer.size()),
    buffer.data(),
    nullptr);
  if (written == 0 || written >= buffer.size()) {
    return {};
  }
  buffer.resize(written);
  return std::filesystem::path(std::move(buffer)).lexically_normal();
}

void normalize_optional_root(std::filesystem::path& path) {
  if (path.empty()) {
    return;
  }
  std::error_code error;
  auto absolute = path.is_absolute() ? path : std::filesystem::absolute(path, error);
  if (!error) {
    path = absolute.lexically_normal();
  }
}

bool has_reparse_component(const std::filesystem::path& path) {
  auto current = path.root_path();
  for (const auto& component : path.relative_path()) {
    current /= component;
    const auto attributes = GetFileAttributesW(current.wstring().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return true;
    }
  }
  return false;
}

void validate_policy_paths(const std::vector<std::filesystem::path>& paths, std::string_view field,
  std::vector<std::string>& blockers) {
  for (std::size_t index = 0; index < paths.size(); ++index) {
    const auto& path = paths[index];
    bool valid = true;
    auto current = path.root_path();
    for (const auto& component : path.relative_path()) {
      current /= component;
      const auto attributes = GetFileAttributesW(current.wstring().c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES) {
        blockers.push_back(std::string(field) + "[" + std::to_string(index) + "]:path_unavailable");
        valid = false;
        break;
      }
      if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        blockers.push_back(
          std::string(field) + "[" + std::to_string(index) + "]:reparse_point_not_allowed");
        valid = false;
        break;
      }
    }
    if (!valid) {
      continue;
    }

    std::wstring volume_root(32768, L'\0');
    if (!GetVolumePathNameW(
          path.wstring().c_str(), volume_root.data(), static_cast<DWORD>(volume_root.size()))) {
      blockers.push_back(
        std::string(field) + "[" + std::to_string(index) + "]:volume_probe_failed");
      continue;
    }
    DWORD filesystem_flags = 0;
    if (!GetVolumeInformationW(
          volume_root.c_str(), nullptr, 0, nullptr, nullptr, &filesystem_flags, nullptr, 0) ||
        (filesystem_flags & FS_PERSISTENT_ACLS) == 0) {
      blockers.push_back(
        std::string(field) + "[" + std::to_string(index) + "]:persistent_acls_unavailable");
    }
  }
}

std::optional<std::wstring> utf8_to_wide(std::string_view text) {
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

void validate_windows_environment(
  const std::map<std::string, std::string>& environment, std::vector<std::string>& blockers) {
  std::vector<std::wstring> names;
  for (const auto& [name, value] : environment) {
    auto wide_name = utf8_to_wide(name);
    const auto wide_value = utf8_to_wide(value);
    if (!wide_name || !wide_value) {
      blockers.emplace_back("environment_invalid_utf8");
      return;
    }
    if (std::any_of(names.begin(), names.end(), [&](const auto& existing) {
          return equal_component_case_insensitive(existing, *wide_name);
        })) {
      blockers.emplace_back("environment_case_insensitive_name_collision");
      return;
    }
    names.push_back(std::move(*wide_name));
  }
}

sandbox::sandbox_compile_result unsupported(
  std::string message, std::vector<std::string> blockers) {
  return {
    .error = sandbox::sandbox_compile_error::unsupported_policy,
    .message = std::move(message),
    .blockers = std::move(blockers),
  };
}

} // namespace

windows_restricted_process_sandbox_plan::windows_restricted_process_sandbox_plan(
  std::uint64_t plan_id, sandbox::sandbox_policy policy,
  sandbox::sandbox_enforcement_contract enforcement,
  restricted_process_backend_config runtime_config)
    : sandbox_plan("restricted_process", sandbox::sandbox_platform::host_windows, std::move(policy),
        enforcement,
        {
          { "plan_format", "windows_restricted_process" },
          { "plan_format_version", std::to_string(current_format_version) },
          { "plan_id", std::to_string(plan_id) },
          { "filesystem_platform_defaults", "windows_appcontainer_intrinsic" },
        }),
      plan_id_(plan_id), runtime_config_(std::move(runtime_config)) {
}

restricted_path_access windows_restricted_process_sandbox_plan::access_for(
  const std::filesystem::path& path) const {
  if (path.empty()) {
    return restricted_path_access::denied;
  }
  const auto normalized = path.lexically_normal();
  for (const auto& root : policy().filesystem.denied_paths) {
    if (path_contains_case_insensitive(root, normalized)) {
      return restricted_path_access::denied;
    }
  }
  for (const auto& root : policy().filesystem.protected_read_only_paths) {
    if (path_contains_case_insensitive(root, normalized)) {
      return restricted_path_access::protected_read_only;
    }
  }
  for (const auto& root : policy().filesystem.writable_roots) {
    if (path_contains_case_insensitive(root, normalized)) {
      return restricted_path_access::writable;
    }
  }
  for (const auto& root : policy().filesystem.readable_roots) {
    if (path_contains_case_insensitive(root, normalized)) {
      return restricted_path_access::readable;
    }
  }
  return restricted_path_access::denied;
}

std::size_t windows_restricted_process_sandbox_plan::constrain_process_count(
  std::size_t requested) const noexcept {
  return constrain_nonzero(requested, policy().resources.max_process_count);
}

std::uint64_t windows_restricted_process_sandbox_plan::constrain_memory_bytes(
  std::uint64_t requested) const noexcept {
  return constrain_nonzero(requested, policy().resources.max_memory_bytes);
}

std::chrono::milliseconds windows_restricted_process_sandbox_plan::constrain_cpu_time(
  std::chrono::milliseconds requested) const noexcept {
  return constrain_nonzero(requested, policy().resources.max_cpu_time);
}

sandbox::sandbox_compile_result compile_windows_restricted_process_sandbox_policy(
  const sandbox::sandbox_policy& policy, const sandbox::sandbox_backend_info& backend,
  restricted_process_backend_config config) {
  auto generic =
    sandbox::compile_sandbox_policy(policy, backend, sandbox::sandbox_platform::host_windows);
  if (!generic) {
    return generic;
  }

  const auto& normalized = generic.plan->policy();
  std::vector<std::string> blockers;
  if (normalized.filesystem.read_access != sandbox::sandbox_filesystem_read_access::restricted) {
    blockers.emplace_back("filesystem_full_read_unsupported");
  }
  if (!normalized.filesystem.include_platform_defaults) {
    blockers.emplace_back("filesystem_platform_defaults_required");
  }
  if (normalized.network.mode != sandbox::sandbox_network_mode::denied) {
    blockers.emplace_back("network_mode_unsupported");
  }
  if (normalized.network.allow_local_binding) {
    blockers.emplace_back("local_binding_unsupported");
  }
  if (!config.use_job_object) {
    blockers.emplace_back("job_object_required");
  }
  if (!config.deny_network) {
    blockers.emplace_back("network_deny_required");
  }
  if (normalized.resources.max_process_count &&
      *normalized.resources.max_process_count > (std::numeric_limits<DWORD>::max)()) {
    blockers.emplace_back("max_process_count_exceeds_windows_limit");
  }
  if (normalized.resources.max_memory_bytes &&
      *normalized.resources.max_memory_bytes > (std::numeric_limits<SIZE_T>::max)()) {
    blockers.emplace_back("max_memory_bytes_exceeds_windows_limit");
  }
  if (normalized.resources.max_cpu_time && normalized.resources.max_cpu_time->count() >
                                             (std::numeric_limits<LONGLONG>::max)() / 10000LL) {
    blockers.emplace_back("max_cpu_time_exceeds_windows_limit");
  }
  validate_policy_paths(
    normalized.filesystem.readable_roots, "filesystem.readable_roots", blockers);
  validate_policy_paths(
    normalized.filesystem.writable_roots, "filesystem.writable_roots", blockers);
  validate_policy_paths(normalized.filesystem.protected_read_only_paths,
    "filesystem.protected_read_only_paths",
    blockers);
  validate_policy_paths(normalized.filesystem.denied_paths, "filesystem.denied_paths", blockers);
  validate_windows_environment(normalized.environment.variables, blockers);
  if (!blockers.empty()) {
    return unsupported("Windows restricted_process cannot preserve the requested sandbox semantics",
      std::move(blockers));
  }

  config.python_interpreter = resolve_python_executable(config.python_interpreter);
  if (config.python_interpreter.empty()) {
    return {
      .error = sandbox::sandbox_compile_error::backend_unavailable,
      .message = "configured Python interpreter is unavailable",
      .blockers = { "python_interpreter_unavailable" },
    };
  }
  if (has_reparse_component(config.python_interpreter)) {
    return {
      .error = sandbox::sandbox_compile_error::backend_unavailable,
      .message = "configured Python interpreter traverses a reparse point",
      .blockers = { "python_interpreter_reparse_point_not_allowed" },
    };
  }

  normalize_optional_root(config.fallback_workdir);
  normalize_optional_root(config.runtime_staging_root);
  config.readable_roots = normalized.filesystem.readable_roots;
  config.writable_roots = normalized.filesystem.writable_roots;
  config.base_environment = normalized.environment.variables;
  config.inherit_parent_environment = normalized.environment.inherit_parent;
  config.use_job_object = true;
  config.deny_network = true;

  auto normalized_policy = normalized;
  auto plan_enforcement = backend.enforcement;
  if (normalized_policy.environment.inherit_parent) {
    plan_enforcement.environment_allowlist = sandbox::enforcement_level::not_applicable;
  }
  const auto id = next_plan_id.fetch_add(1, std::memory_order_relaxed);
  return {
    .plan = std::shared_ptr<const windows_restricted_process_sandbox_plan>(
      new windows_restricted_process_sandbox_plan(
        id, std::move(normalized_policy), plan_enforcement, std::move(config))),
  };
}

std::shared_ptr<const windows_restricted_process_sandbox_plan>
as_windows_restricted_process_sandbox_plan(
  const std::shared_ptr<const sandbox::sandbox_plan>& plan) noexcept {
  return std::dynamic_pointer_cast<const windows_restricted_process_sandbox_plan>(plan);
}

} // namespace wuwe::agent::execution::detail

#endif // _WIN32
