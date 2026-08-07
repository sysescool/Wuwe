#include <wuwe/agent/sandbox/sandbox_codec.hpp>

#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace wuwe::agent::sandbox {
namespace {

void reject_unknown_fields(const nlohmann::json& encoded,
  std::initializer_list<std::string_view> allowed, std::string_view context) {
  if (!encoded.is_object()) {
    throw std::invalid_argument(std::string(context) + " must be a JSON object");
  }
  for (const auto& [key, value] : encoded.items()) {
    (void)value;
    if (std::find(allowed.begin(), allowed.end(), std::string_view(key)) == allowed.end()) {
      throw std::invalid_argument(std::string(context) + " contains unknown field: " + key);
    }
  }
}

isolation_level isolation_from_string(const std::string& value) {
  if (value == "none")
    return isolation_level::none;
  if (value == "controlled_process")
    return isolation_level::controlled_process;
  if (value == "restricted_process")
    return isolation_level::restricted_process;
  if (value == "container")
    return isolation_level::container;
  if (value == "wasm")
    return isolation_level::wasm;
  throw std::invalid_argument("invalid sandbox isolation level: " + value);
}

sandbox_filesystem_read_access filesystem_read_access_from_string(const std::string& value) {
  if (value == "full")
    return sandbox_filesystem_read_access::full;
  if (value == "restricted")
    return sandbox_filesystem_read_access::restricted;
  throw std::invalid_argument("invalid sandbox filesystem read access: " + value);
}

sandbox_network_mode network_mode_from_string(const std::string& value) {
  if (value == "denied")
    return sandbox_network_mode::denied;
  if (value == "unrestricted")
    return sandbox_network_mode::unrestricted;
  if (value == "filtered")
    return sandbox_network_mode::filtered;
  throw std::invalid_argument("invalid sandbox network mode: " + value);
}

sandbox_network_action network_action_from_string(const std::string& value) {
  if (value == "allow")
    return sandbox_network_action::allow;
  if (value == "deny")
    return sandbox_network_action::deny;
  throw std::invalid_argument("invalid sandbox network rule action: " + value);
}

nlohmann::json paths_to_json(const std::vector<std::filesystem::path>& paths) {
  nlohmann::json output = nlohmann::json::array();
  for (const auto& path : paths) {
    output.push_back(path.generic_string());
  }
  return output;
}

std::vector<std::filesystem::path> paths_from_json(const nlohmann::json& encoded) {
  std::vector<std::filesystem::path> paths;
  for (const auto& item : encoded) {
    paths.emplace_back(item.get<std::string>());
  }
  return paths;
}

nlohmann::json enforcement_requirements_to_json(const sandbox_enforcement_requirements& value) {
  return {
    { "shell_execution", value.shell_execution },
    { "timeout", value.timeout },
    { "cancellation", value.cancellation },
    { "stdout_limit", value.stdout_limit },
    { "stderr_limit", value.stderr_limit },
    { "environment_allowlist", value.environment_allowlist },
    { "working_directory", value.working_directory },
    { "process_tree_cleanup", value.process_tree_cleanup },
    { "process_count_limit", value.process_count_limit },
    { "cpu_time_limit", value.cpu_time_limit },
    { "memory_limit", value.memory_limit },
    { "filesystem_read_deny", value.filesystem_read_deny },
    { "filesystem_write_deny", value.filesystem_write_deny },
    { "network_deny", value.network_deny },
    { "network_filter", value.network_filter },
  };
}

sandbox_enforcement_requirements enforcement_requirements_from_json(const nlohmann::json& encoded) {
  reject_unknown_fields(encoded,
    { "shell_execution",
      "timeout",
      "cancellation",
      "stdout_limit",
      "stderr_limit",
      "environment_allowlist",
      "working_directory",
      "process_tree_cleanup",
      "process_count_limit",
      "cpu_time_limit",
      "memory_limit",
      "filesystem_read_deny",
      "filesystem_write_deny",
      "network_deny",
      "network_filter" },
    "sandbox required_enforcement");
  return {
    .shell_execution = encoded.value("shell_execution", false),
    .timeout = encoded.value("timeout", false),
    .cancellation = encoded.value("cancellation", false),
    .stdout_limit = encoded.value("stdout_limit", false),
    .stderr_limit = encoded.value("stderr_limit", false),
    .environment_allowlist = encoded.value("environment_allowlist", false),
    .working_directory = encoded.value("working_directory", false),
    .process_tree_cleanup = encoded.value("process_tree_cleanup", false),
    .process_count_limit = encoded.value("process_count_limit", false),
    .cpu_time_limit = encoded.value("cpu_time_limit", false),
    .memory_limit = encoded.value("memory_limit", false),
    .filesystem_read_deny = encoded.value("filesystem_read_deny", false),
    .filesystem_write_deny = encoded.value("filesystem_write_deny", false),
    .network_deny = encoded.value("network_deny", false),
    .network_filter = encoded.value("network_filter", false),
  };
}

} // namespace

nlohmann::json sandbox_policy_to_json(const sandbox_policy& policy) {
  nlohmann::json rules = nlohmann::json::array();
  for (const auto& rule : policy.network.rules) {
    rules.push_back({
      { "host_pattern", rule.host_pattern },
      { "action", to_string(rule.action) },
      { "port", rule.port ? nlohmann::json(*rule.port) : nlohmann::json(nullptr) },
    });
  }
  return {
    { "schema_version", 1 },
    { "name", policy.name },
    { "required_isolation",
      policy.required_isolation ? nlohmann::json(to_string(*policy.required_isolation))
                                : nlohmann::json(nullptr) },
    { "filesystem",
      {
        { "read_access", to_string(policy.filesystem.read_access) },
        { "readable_roots", paths_to_json(policy.filesystem.readable_roots) },
        { "writable_roots", paths_to_json(policy.filesystem.writable_roots) },
        { "protected_read_only_paths", paths_to_json(policy.filesystem.protected_read_only_paths) },
        { "denied_paths", paths_to_json(policy.filesystem.denied_paths) },
        { "include_platform_defaults", policy.filesystem.include_platform_defaults },
      } },
    { "network",
      {
        { "mode", to_string(policy.network.mode) },
        { "default_action", to_string(policy.network.default_action) },
        { "rules", std::move(rules) },
        { "allow_local_binding", policy.network.allow_local_binding },
      } },
    { "environment",
      {
        { "inherit_parent", policy.environment.inherit_parent },
        { "variables", policy.environment.variables },
      } },
    { "resources",
      {
        { "cleanup_process_tree", policy.resources.cleanup_process_tree },
        { "max_process_count",
          policy.resources.max_process_count ? nlohmann::json(*policy.resources.max_process_count)
                                             : nlohmann::json(nullptr) },
        { "max_memory_bytes",
          policy.resources.max_memory_bytes ? nlohmann::json(*policy.resources.max_memory_bytes)
                                            : nlohmann::json(nullptr) },
        { "max_cpu_time_ms",
          policy.resources.max_cpu_time ? nlohmann::json(policy.resources.max_cpu_time->count())
                                        : nlohmann::json(nullptr) },
      } },
    { "required_enforcement", enforcement_requirements_to_json(policy.required_enforcement) },
    { "metadata", policy.metadata },
  };
}

sandbox_policy sandbox_policy_from_json(const nlohmann::json& encoded) {
  reject_unknown_fields(encoded,
    { "schema_version",
      "name",
      "required_isolation",
      "filesystem",
      "network",
      "environment",
      "resources",
      "required_enforcement",
      "metadata" },
    "sandbox policy");
  if (encoded.value("schema_version", 0) != 1) {
    throw std::invalid_argument("unsupported sandbox policy schema version");
  }
  const auto& filesystem = encoded.at("filesystem");
  const auto& network = encoded.at("network");
  const auto& environment = encoded.at("environment");
  const auto& resources = encoded.at("resources");
  reject_unknown_fields(filesystem,
    { "read_access",
      "readable_roots",
      "writable_roots",
      "protected_read_only_paths",
      "denied_paths",
      "include_platform_defaults" },
    "sandbox filesystem policy");
  reject_unknown_fields(network,
    { "mode", "default_action", "rules", "allow_local_binding" },
    "sandbox network policy");
  reject_unknown_fields(
    environment, { "inherit_parent", "variables" }, "sandbox environment policy");
  reject_unknown_fields(resources,
    { "cleanup_process_tree", "max_process_count", "max_memory_bytes", "max_cpu_time_ms" },
    "sandbox resource policy");
  sandbox_policy policy;
  policy.name = encoded.value("name", std::string {});
  if (encoded.contains("required_isolation") && !encoded.at("required_isolation").is_null()) {
    policy.required_isolation =
      isolation_from_string(encoded.at("required_isolation").get<std::string>());
  }
  else {
    policy.required_isolation.reset();
  }
  policy.filesystem = {
    .read_access = filesystem_read_access_from_string(
      filesystem.value("read_access", std::string("restricted"))),
    .readable_roots = paths_from_json(filesystem.value("readable_roots", nlohmann::json::array())),
    .writable_roots = paths_from_json(filesystem.value("writable_roots", nlohmann::json::array())),
    .protected_read_only_paths =
      paths_from_json(filesystem.value("protected_read_only_paths", nlohmann::json::array())),
    .denied_paths = paths_from_json(filesystem.value("denied_paths", nlohmann::json::array())),
    .include_platform_defaults = filesystem.value("include_platform_defaults", true),
  };
  policy.network.mode = network_mode_from_string(network.value("mode", std::string("denied")));
  policy.network.default_action =
    network_action_from_string(network.value("default_action", std::string("deny")));
  for (const auto& item : network.value("rules", nlohmann::json::array())) {
    reject_unknown_fields(item, { "host_pattern", "action", "port" }, "sandbox network rule");
    sandbox_network_rule rule {
      .host_pattern = item.at("host_pattern").get<std::string>(),
      .action = network_action_from_string(item.value("action", std::string("allow"))),
    };
    if (item.contains("port") && !item.at("port").is_null()) {
      const auto port = item.at("port").get<std::uint32_t>();
      if (port == 0 || port > 65535) {
        throw std::invalid_argument("sandbox network rule port is outside the valid range");
      }
      rule.port = static_cast<std::uint16_t>(port);
    }
    policy.network.rules.push_back(std::move(rule));
  }
  policy.network.allow_local_binding = network.value("allow_local_binding", false);
  policy.environment = {
    .inherit_parent = environment.value("inherit_parent", false),
    .variables = environment.value("variables", std::map<std::string, std::string> {}),
  };
  policy.resources.cleanup_process_tree = resources.value("cleanup_process_tree", true);
  if (resources.contains("max_process_count") && !resources.at("max_process_count").is_null()) {
    policy.resources.max_process_count = resources.at("max_process_count").get<std::size_t>();
  }
  if (resources.contains("max_memory_bytes") && !resources.at("max_memory_bytes").is_null()) {
    policy.resources.max_memory_bytes = resources.at("max_memory_bytes").get<std::uint64_t>();
  }
  if (resources.contains("max_cpu_time_ms") && !resources.at("max_cpu_time_ms").is_null()) {
    policy.resources.max_cpu_time =
      std::chrono::milliseconds(resources.at("max_cpu_time_ms").get<std::int64_t>());
  }
  policy.required_enforcement = enforcement_requirements_from_json(
    encoded.value("required_enforcement", nlohmann::json::object()));
  policy.metadata = encoded.value("metadata", std::map<std::string, std::string> {});

  auto validation = validate_sandbox_policy(std::move(policy));
  if (!validation) {
    throw std::invalid_argument(
      "persisted sandbox policy is invalid: " + validation.issues.front().message);
  }
  return std::move(validation.normalized);
}

} // namespace wuwe::agent::sandbox
