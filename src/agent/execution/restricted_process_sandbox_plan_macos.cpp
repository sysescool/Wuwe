#include "restricted_process_sandbox_plan_macos.hpp"

#ifdef __APPLE__

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace wuwe::agent::execution::detail {
namespace {

bool contains_control(std::string_view value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char ch) { return ch < 0x20; });
}

std::string seatbelt_string(const std::filesystem::path& path) {
  const auto value = path.string();
  std::string result;
  result.reserve(value.size() + 2);
  result.push_back('"');
  for (const auto ch : value) {
    if (ch == '\\' || ch == '"')
      result.push_back('\\');
    result.push_back(ch);
  }
  result.push_back('"');
  return result;
}

std::filesystem::path resolve_executable(const std::filesystem::path& configured) {
  if (configured.empty())
    return {};
  if (configured.is_absolute() || configured.has_parent_path()) {
    return ::access(configured.c_str(), X_OK) == 0 ? std::filesystem::canonical(configured)
                                                   : std::filesystem::path {};
  }
  const char* path_value = std::getenv("PATH");
  if (!path_value)
    return {};
  std::stringstream paths(path_value);
  std::string directory;
  while (std::getline(paths, directory, ':')) {
    auto candidate = std::filesystem::path(directory.empty() ? "." : directory) / configured;
    if (::access(candidate.c_str(), X_OK) == 0) {
      if (candidate == "/usr/bin/python3") {
        std::error_code developer_error;
        auto developer =
          std::filesystem::read_symlink("/var/select/developer_dir", developer_error);
        if (!developer_error) {
          auto toolchain_python = developer / "usr/bin/python3";
          auto resolved = std::filesystem::canonical(toolchain_python, developer_error);
          if (!developer_error && ::access(resolved.c_str(), X_OK) == 0)
            return resolved;
        }
      }
      std::error_code error;
      auto resolved = std::filesystem::canonical(candidate, error);
      if (!error)
        return resolved;
    }
  }
  return {};
}

bool validate_bound_path(const std::filesystem::path& path) {
  if (contains_control(path.string()))
    return false;
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  if (error || canonical != path)
    return false;
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0 && !S_ISLNK(info.st_mode);
}

std::filesystem::path python_framework_version_root(const std::filesystem::path& interpreter) {
  const auto bin = interpreter.parent_path();
  const auto version_root = bin.parent_path();
  const auto versions = version_root.parent_path();
  const auto framework = versions.parent_path();
  const auto executable_name = interpreter.filename().string();
  if (executable_name.rfind("python", 0) != 0 || bin.filename() != "bin" ||
      versions.filename() != "Versions" || framework.extension() != ".framework") {
    return {};
  }
  return version_root;
}

void append_subpath_rules(std::ostringstream& profile, std::string_view operation,
  const std::vector<std::filesystem::path>& paths) {
  for (const auto& path : paths) {
    profile << "(allow " << operation << " (literal " << seatbelt_string(path) << ") (subpath "
            << seatbelt_string(path) << "))\n";
  }
}

void append_subpath_denials(std::ostringstream& profile, std::string_view operation,
  const std::vector<std::filesystem::path>& paths) {
  for (const auto& path : paths) {
    profile << "(deny " << operation << " (literal " << seatbelt_string(path) << ") (subpath "
            << seatbelt_string(path) << "))\n";
  }
}

std::string build_profile(const sandbox::sandbox_policy& policy) {
  std::ostringstream profile;
  profile << "(version 1)\n"
             "(deny default)\n"
             "(allow process*)\n"
             "(allow signal (target same-sandbox))\n"
             "(allow sysctl-read)\n"
             "(allow file-write-data (require-not (vnode-type REGULAR-FILE)))\n"
             "(allow file-read-data (literal \"/\"))\n"
             "(allow file-read-metadata (subpath \"/\"))\n";

  if (policy.filesystem.include_platform_defaults) {
    for (const auto* root :
      { "/System", "/usr", "/Library/Apple", "/Library/Developer", "/private/var/db/dyld" }) {
      profile << "(allow file-read* (literal \"" << root << "\") (subpath \"" << root << "\"))\n";
    }
    profile << "(allow file-read* (literal \"/dev/null\") (literal \"/dev/urandom\") "
               "(literal \"/dev/random\"))\n";
  }
  append_subpath_rules(profile, "file-read*", policy.filesystem.readable_roots);
  append_subpath_rules(profile, "file-read*", policy.filesystem.writable_roots);
  append_subpath_rules(profile, "file-write*", policy.filesystem.writable_roots);
  append_subpath_rules(profile, "file-read*", policy.filesystem.protected_read_only_paths);
  append_subpath_denials(profile, "file-write*", policy.filesystem.protected_read_only_paths);
  append_subpath_denials(profile, "file-read*", policy.filesystem.denied_paths);
  append_subpath_denials(profile, "file-write*", policy.filesystem.denied_paths);
  if (policy.network.mode == sandbox::sandbox_network_mode::unrestricted) {
    profile << "(allow network*)\n";
  }
  return profile.str();
}

} // namespace

macos_restricted_process_sandbox_plan::macos_restricted_process_sandbox_plan(
  sandbox::sandbox_policy policy, sandbox::sandbox_enforcement_contract enforcement,
  restricted_process_backend_config config, std::string profile)
    : sandbox_plan("restricted_process", sandbox::sandbox_platform::host_macos, std::move(policy),
        enforcement,
        { { "plan_format", "macos_seatbelt_restricted_process" },
          { "plan_format_version", std::to_string(current_format_version) },
          { "filesystem_platform_defaults", "macos_runtime_read_only" },
          { "launcher", "apple_sandbox_exec" } }),
      runtime_config_(std::move(config)), seatbelt_profile_(std::move(profile)) {
}

sandbox::sandbox_compile_result compile_macos_restricted_process_sandbox_policy(
  const sandbox::sandbox_policy& policy, const sandbox::sandbox_backend_info& backend,
  restricted_process_backend_config config) {
  auto generic =
    sandbox::compile_sandbox_policy(policy, backend, sandbox::sandbox_platform::host_macos);
  if (!generic)
    return generic;

  auto normalized = generic.plan->policy();
  std::vector<std::string> blockers;
  if (normalized.filesystem.read_access != sandbox::sandbox_filesystem_read_access::restricted)
    blockers.emplace_back("filesystem_full_read_unsupported");
  if (!normalized.filesystem.include_platform_defaults)
    blockers.emplace_back("filesystem_platform_defaults_required");
  if (normalized.network.mode == sandbox::sandbox_network_mode::filtered)
    blockers.emplace_back("network_filter_unsupported");
  if (normalized.network.allow_local_binding)
    blockers.emplace_back("local_binding_unsupported");
  if (!config.use_process_group)
    blockers.emplace_back("process_group_required");
  if (normalized.environment.inherit_parent)
    blockers.emplace_back("parent_environment_inheritance_unsupported");

  config.seatbelt_executable = resolve_executable(config.seatbelt_executable);
  if (config.seatbelt_executable != "/usr/bin/sandbox-exec")
    blockers.emplace_back("system_seatbelt_launcher_required");
  config.python_interpreter = resolve_executable(config.python_interpreter);
  if (config.python_interpreter.empty())
    blockers.emplace_back("python_interpreter_unavailable");
  const auto python_runtime_root = python_framework_version_root(config.python_interpreter);
  if (!python_runtime_root.empty() && !validate_bound_path(python_runtime_root))
    blockers.emplace_back("python_framework_runtime_unavailable");

  if (config.fallback_workdir.empty())
    config.fallback_workdir = std::filesystem::temp_directory_path() / "wuwe-restricted";
  std::error_code error;
  std::filesystem::create_directories(config.fallback_workdir, error);
  config.fallback_workdir = std::filesystem::canonical(config.fallback_workdir, error);
  if (error || !validate_bound_path(config.fallback_workdir))
    blockers.emplace_back("fallback_workdir_unavailable");

  auto validate_paths = [&](
                          const std::vector<std::filesystem::path>& paths, std::string_view field) {
    for (std::size_t index = 0; index < paths.size(); ++index) {
      if (!validate_bound_path(paths[index]))
        blockers.emplace_back(
          std::string(field) + "[" + std::to_string(index) + "]:noncanonical_or_symlink_path");
    }
  };
  validate_paths(normalized.filesystem.readable_roots, "filesystem.readable_roots");
  validate_paths(normalized.filesystem.writable_roots, "filesystem.writable_roots");
  validate_paths(
    normalized.filesystem.protected_read_only_paths, "filesystem.protected_read_only_paths");
  validate_paths(normalized.filesystem.denied_paths, "filesystem.denied_paths");
  if (!blockers.empty()) {
    return { .error = sandbox::sandbox_compile_error::unsupported_policy,
      .message = "macOS Seatbelt cannot safely preserve the requested policy",
      .blockers = std::move(blockers) };
  }

  auto add_unique = [](auto& paths, const auto& path) {
    if (std::find(paths.begin(), paths.end(), path) == paths.end())
      paths.push_back(path);
  };
  add_unique(normalized.filesystem.readable_roots, config.python_interpreter);
  if (!python_runtime_root.empty())
    add_unique(normalized.filesystem.readable_roots, python_runtime_root);
  add_unique(normalized.filesystem.writable_roots, config.fallback_workdir);
  config.readable_roots = normalized.filesystem.readable_roots;
  config.writable_roots = normalized.filesystem.writable_roots;
  config.base_environment = normalized.environment.variables;
  config.inherit_parent_environment = normalized.environment.inherit_parent;

  auto profile = build_profile(normalized);
  return {
    .plan = std::shared_ptr<const sandbox::sandbox_plan>(new macos_restricted_process_sandbox_plan(
      std::move(normalized), backend.enforcement, std::move(config), std::move(profile)))
  };
}

std::shared_ptr<const macos_restricted_process_sandbox_plan>
as_macos_restricted_process_sandbox_plan(
  const std::shared_ptr<const sandbox::sandbox_plan>& plan) noexcept {
  return std::dynamic_pointer_cast<const macos_restricted_process_sandbox_plan>(plan);
}

} // namespace wuwe::agent::execution::detail
#endif
