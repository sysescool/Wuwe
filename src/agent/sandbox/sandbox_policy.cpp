#include <wuwe/agent/sandbox/sandbox_policy.hpp>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace wuwe::agent::sandbox {
namespace {

bool contains_nul(std::string_view value) {
  return value.find('\0') != std::string_view::npos;
}

void add_issue(sandbox_policy_validation& result, sandbox_policy_error error, std::string field,
  std::string message) {
  result.issues.push_back({
    .error = error,
    .field = std::move(field),
    .message = std::move(message),
  });
}

void normalize_paths(std::vector<std::filesystem::path>& paths, std::string_view field,
  sandbox_policy_validation& result) {
  std::vector<std::filesystem::path> normalized;
  normalized.reserve(paths.size());
  for (std::size_t index = 0; index < paths.size(); ++index) {
    auto path = paths[index];
    const auto item_field = std::string(field) + "[" + std::to_string(index) + "]";
    if (path.empty()) {
      add_issue(
        result, sandbox_policy_error::invalid_path, item_field, "sandbox paths must not be empty");
      continue;
    }
    if (!path.is_absolute()) {
      add_issue(
        result, sandbox_policy_error::invalid_path, item_field, "sandbox paths must be absolute");
      continue;
    }
    path = path.lexically_normal();
    if (std::find(normalized.begin(), normalized.end(), path) == normalized.end()) {
      normalized.push_back(std::move(path));
    }
  }
  paths = std::move(normalized);
}

bool valid_host_pattern(std::string_view pattern) {
  if (pattern.empty() || contains_nul(pattern) || pattern.find("://") != std::string_view::npos ||
      pattern.find_first_of("/\\?#") != std::string_view::npos ||
      std::any_of(
        pattern.begin(), pattern.end(), [](unsigned char ch) { return std::isspace(ch) != 0; })) {
    return false;
  }
  if (pattern == "*") {
    return true;
  }
  std::size_t prefix = 0;
  if (pattern.starts_with("**.")) {
    prefix = 3;
  }
  else if (pattern.starts_with("*.")) {
    prefix = 2;
  }
  if (prefix != 0 && prefix == pattern.size()) {
    return false;
  }
  return pattern.find('*', prefix) == std::string_view::npos;
}

} // namespace

std::string to_string(sandbox_filesystem_read_access access) {
  switch (access) {
    case sandbox_filesystem_read_access::full:
      return "full";
    case sandbox_filesystem_read_access::restricted:
      return "restricted";
  }
  return "restricted";
}

std::string to_string(sandbox_network_mode mode) {
  switch (mode) {
    case sandbox_network_mode::denied:
      return "denied";
    case sandbox_network_mode::unrestricted:
      return "unrestricted";
    case sandbox_network_mode::filtered:
      return "filtered";
  }
  return "denied";
}

std::string to_string(sandbox_network_action action) {
  switch (action) {
    case sandbox_network_action::allow:
      return "allow";
    case sandbox_network_action::deny:
      return "deny";
  }
  return "deny";
}

std::string to_string(sandbox_policy_error error) {
  switch (error) {
    case sandbox_policy_error::none:
      return "none";
    case sandbox_policy_error::invalid_name:
      return "invalid_name";
    case sandbox_policy_error::invalid_path:
      return "invalid_path";
    case sandbox_policy_error::invalid_environment:
      return "invalid_environment";
    case sandbox_policy_error::invalid_network_rule:
      return "invalid_network_rule";
    case sandbox_policy_error::ambiguous_network_policy:
      return "ambiguous_network_policy";
    case sandbox_policy_error::invalid_resource_limit:
      return "invalid_resource_limit";
  }
  return "invalid_name";
}

sandbox_policy_validation validate_sandbox_policy(sandbox_policy policy) {
  sandbox_policy_validation result { .normalized = std::move(policy) };
  if (contains_nul(result.normalized.name) || result.normalized.name.size() > 128) {
    add_issue(result,
      sandbox_policy_error::invalid_name,
      "name",
      "sandbox policy names must be at most 128 bytes and contain no NUL characters");
  }

  normalize_paths(result.normalized.filesystem.readable_roots, "filesystem.readable_roots", result);
  normalize_paths(result.normalized.filesystem.writable_roots, "filesystem.writable_roots", result);
  normalize_paths(result.normalized.filesystem.protected_read_only_paths,
    "filesystem.protected_read_only_paths",
    result);
  normalize_paths(result.normalized.filesystem.denied_paths, "filesystem.denied_paths", result);

  for (const auto& [name, value] : result.normalized.environment.variables) {
    if (name.empty() || name.find('=') != std::string::npos || contains_nul(name) ||
        contains_nul(value)) {
      add_issue(result,
        sandbox_policy_error::invalid_environment,
        "environment.variables." + name,
        "environment names must be non-empty and values must not contain NUL characters");
    }
  }
  if (result.normalized.environment.inherit_parent &&
      result.normalized.required_enforcement.environment_allowlist) {
    add_issue(result,
      sandbox_policy_error::invalid_environment,
      "required_enforcement.environment_allowlist",
      "environment allowlist enforcement conflicts with parent environment inheritance");
  }

  if (result.normalized.network.mode != sandbox_network_mode::filtered &&
      !result.normalized.network.rules.empty()) {
    add_issue(result,
      sandbox_policy_error::ambiguous_network_policy,
      "network.rules",
      "network rules are valid only when network mode is filtered");
  }
  if (result.normalized.network.mode != sandbox_network_mode::filtered &&
      result.normalized.network.default_action != sandbox_network_action::deny) {
    add_issue(result,
      sandbox_policy_error::ambiguous_network_policy,
      "network.default_action",
      "network default_action is configurable only when network mode is filtered");
  }
  for (std::size_t index = 0; index < result.normalized.network.rules.size(); ++index) {
    const auto& rule = result.normalized.network.rules[index];
    if (!valid_host_pattern(rule.host_pattern) || (rule.port.has_value() && *rule.port == 0)) {
      add_issue(result,
        sandbox_policy_error::invalid_network_rule,
        "network.rules[" + std::to_string(index) + "]",
        "network rules require a valid host pattern and a non-zero optional port");
    }
  }

  if ((result.normalized.resources.max_process_count &&
        *result.normalized.resources.max_process_count == 0) ||
      (result.normalized.resources.max_memory_bytes &&
        *result.normalized.resources.max_memory_bytes == 0) ||
      (result.normalized.resources.max_cpu_time &&
        *result.normalized.resources.max_cpu_time <= std::chrono::milliseconds::zero())) {
    add_issue(result,
      sandbox_policy_error::invalid_resource_limit,
      "resources",
      "configured sandbox resource limits must be greater than zero");
  }
  return result;
}

} // namespace wuwe::agent::sandbox
