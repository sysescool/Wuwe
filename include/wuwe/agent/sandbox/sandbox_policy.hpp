#ifndef WUWE_AGENT_SANDBOX_SANDBOX_POLICY_HPP
#define WUWE_AGENT_SANDBOX_SANDBOX_POLICY_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <wuwe/agent/sandbox/sandbox.hpp>

namespace wuwe::agent::sandbox {

enum class sandbox_filesystem_read_access {
  full,
  restricted,
};

enum class sandbox_network_mode {
  denied,
  unrestricted,
  filtered,
};

enum class sandbox_network_action {
  allow,
  deny,
};

struct sandbox_network_rule {
  std::string host_pattern;
  sandbox_network_action action { sandbox_network_action::allow };
  std::optional<std::uint16_t> port;
};

struct sandbox_filesystem_policy {
  sandbox_filesystem_read_access read_access { sandbox_filesystem_read_access::restricted };
  std::vector<std::filesystem::path> readable_roots;
  std::vector<std::filesystem::path> writable_roots;
  std::vector<std::filesystem::path> protected_read_only_paths;
  std::vector<std::filesystem::path> denied_paths;
  // Allow unavoidable OS/runtime resources disclosed by the compiled native plan.
  // Backends must reject false when they cannot remove those defaults exactly.
  bool include_platform_defaults { true };
};

struct sandbox_network_policy {
  sandbox_network_mode mode { sandbox_network_mode::denied };
  sandbox_network_action default_action { sandbox_network_action::deny };
  std::vector<sandbox_network_rule> rules;
  bool allow_local_binding { false };
};

struct sandbox_environment_policy {
  bool inherit_parent { false };
  std::map<std::string, std::string> variables;
};

struct sandbox_resource_policy {
  bool cleanup_process_tree { true };
  std::optional<std::size_t> max_process_count;
  std::optional<std::uint64_t> max_memory_bytes;
  std::optional<std::chrono::milliseconds> max_cpu_time;
};

struct sandbox_enforcement_requirements {
  bool shell_execution { false };
  bool timeout { false };
  bool cancellation { false };
  bool stdout_limit { false };
  bool stderr_limit { false };
  bool environment_allowlist { false };
  bool working_directory { false };
  bool process_tree_cleanup { false };
  bool process_count_limit { false };
  bool cpu_time_limit { false };
  bool memory_limit { false };
  bool filesystem_read_deny { false };
  bool filesystem_write_deny { false };
  bool network_deny { false };
  bool network_filter { false };
};

struct sandbox_policy {
  std::string name;
  std::optional<isolation_level> required_isolation { isolation_level::restricted_process };
  sandbox_filesystem_policy filesystem;
  sandbox_network_policy network;
  sandbox_environment_policy environment;
  sandbox_resource_policy resources;
  sandbox_enforcement_requirements required_enforcement;
  std::map<std::string, std::string> metadata;
};

enum class sandbox_policy_error {
  none,
  invalid_name,
  invalid_path,
  invalid_environment,
  invalid_network_rule,
  ambiguous_network_policy,
  invalid_resource_limit,
};

struct sandbox_policy_issue {
  sandbox_policy_error error { sandbox_policy_error::none };
  std::string field;
  std::string message;
};

struct sandbox_policy_validation {
  sandbox_policy normalized;
  std::vector<sandbox_policy_issue> issues;

  [[nodiscard]] explicit operator bool() const noexcept {
    return issues.empty();
  }
};

[[nodiscard]] std::string to_string(sandbox_filesystem_read_access access);
[[nodiscard]] std::string to_string(sandbox_network_mode mode);
[[nodiscard]] std::string to_string(sandbox_network_action action);
[[nodiscard]] std::string to_string(sandbox_policy_error error);

[[nodiscard]] sandbox_policy_validation validate_sandbox_policy(sandbox_policy policy);

} // namespace wuwe::agent::sandbox

#endif // WUWE_AGENT_SANDBOX_SANDBOX_POLICY_HPP
