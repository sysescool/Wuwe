#include <wuwe/agent/sandbox/sandbox_plan.hpp>

#include <string_view>
#include <utility>

namespace wuwe::agent::sandbox {
namespace {

class logical_sandbox_plan final : public sandbox_plan {
public:
  logical_sandbox_plan(std::string backend_name, sandbox_platform platform, sandbox_policy policy,
    sandbox_enforcement_contract enforcement)
      : sandbox_plan(std::move(backend_name), platform, std::move(policy), enforcement) {
  }
};

void require_enforced(bool required, enforcement_level actual, std::string_view name,
  std::vector<std::string>& blockers) {
  if (required && actual != enforcement_level::enforced) {
    blockers.emplace_back(name);
  }
}

sandbox_enforcement_requirements effective_requirements(const sandbox_policy& policy) {
  auto required = policy.required_enforcement;
  const auto& filesystem = policy.filesystem;
  if (filesystem.read_access == sandbox_filesystem_read_access::restricted ||
      !filesystem.readable_roots.empty() || !filesystem.denied_paths.empty()) {
    required.filesystem_read_deny = true;
  }
  if (!filesystem.writable_roots.empty() || !filesystem.protected_read_only_paths.empty() ||
      !filesystem.denied_paths.empty()) {
    required.filesystem_write_deny = true;
  }
  if (!policy.environment.inherit_parent) {
    required.environment_allowlist = true;
  }
  if (policy.resources.cleanup_process_tree) {
    required.process_tree_cleanup = true;
  }
  if (policy.resources.max_process_count) {
    required.process_count_limit = true;
  }
  if (policy.resources.max_memory_bytes) {
    required.memory_limit = true;
  }
  if (policy.resources.max_cpu_time) {
    required.cpu_time_limit = true;
  }
  if (policy.network.mode == sandbox_network_mode::denied) {
    required.network_deny = true;
  }
  else if (policy.network.mode == sandbox_network_mode::filtered) {
    required.network_deny = true;
    required.network_filter = true;
  }
  return required;
}

std::vector<std::string> enforcement_blockers(
  const sandbox_enforcement_requirements& required, const sandbox_enforcement_contract& actual) {
  std::vector<std::string> blockers;
  require_enforced(
    required.shell_execution, actual.shell_execution, "shell_execution_not_enforced", blockers);
  require_enforced(required.timeout, actual.timeout, "timeout_not_enforced", blockers);
  require_enforced(
    required.cancellation, actual.cancellation, "cancellation_not_enforced", blockers);
  require_enforced(
    required.stdout_limit, actual.stdout_limit, "stdout_limit_not_enforced", blockers);
  require_enforced(
    required.stderr_limit, actual.stderr_limit, "stderr_limit_not_enforced", blockers);
  require_enforced(required.environment_allowlist,
    actual.environment_allowlist,
    "environment_allowlist_not_enforced",
    blockers);
  require_enforced(required.working_directory,
    actual.working_directory,
    "working_directory_not_enforced",
    blockers);
  require_enforced(required.process_tree_cleanup,
    actual.process_tree_cleanup,
    "process_tree_cleanup_not_enforced",
    blockers);
  require_enforced(required.process_count_limit,
    actual.process_count_limit,
    "process_count_limit_not_enforced",
    blockers);
  require_enforced(
    required.cpu_time_limit, actual.cpu_time_limit, "cpu_time_limit_not_enforced", blockers);
  require_enforced(
    required.memory_limit, actual.memory_limit, "memory_limit_not_enforced", blockers);
  require_enforced(required.filesystem_read_deny,
    actual.filesystem_read_deny,
    "filesystem_read_deny_not_enforced",
    blockers);
  require_enforced(required.filesystem_write_deny,
    actual.filesystem_write_deny,
    "filesystem_write_deny_not_enforced",
    blockers);
  require_enforced(
    required.network_deny, actual.network_deny, "network_deny_not_enforced", blockers);
  require_enforced(
    required.network_filter, actual.network_filter, "network_filter_not_enforced", blockers);
  return blockers;
}

} // namespace

sandbox_plan::sandbox_plan(std::string backend_name, sandbox_platform platform,
  sandbox_policy policy, sandbox_enforcement_contract enforcement,
  std::map<std::string, std::string> metadata)
    : backend_name_(std::move(backend_name)), platform_(platform), policy_(std::move(policy)),
      enforcement_(enforcement), metadata_(std::move(metadata)) {
}

std::string to_string(sandbox_compile_error error) {
  switch (error) {
    case sandbox_compile_error::none:
      return "none";
    case sandbox_compile_error::invalid_policy:
      return "invalid_policy";
    case sandbox_compile_error::backend_unavailable:
      return "backend_unavailable";
    case sandbox_compile_error::unsupported_isolation:
      return "unsupported_isolation";
    case sandbox_compile_error::unsupported_policy:
      return "unsupported_policy";
  }
  return "invalid_policy";
}

sandbox_compile_result compile_sandbox_policy(
  const sandbox_policy& policy, const sandbox_backend_info& backend, sandbox_platform platform) {
  auto validation = validate_sandbox_policy(policy);
  if (!validation) {
    std::vector<std::string> blockers;
    blockers.reserve(validation.issues.size());
    for (const auto& issue : validation.issues) {
      blockers.push_back(to_string(issue.error) + ":" + issue.field);
    }
    return {
      .error = sandbox_compile_error::invalid_policy,
      .message = validation.issues.front().message,
      .blockers = std::move(blockers),
    };
  }
  if (!backend.available) {
    return {
      .error = sandbox_compile_error::backend_unavailable,
      .message = backend.unavailable_reason.empty() ? "sandbox backend is unavailable"
                                                    : backend.unavailable_reason,
      .blockers = backend.unavailable_reason.empty()
                    ? std::vector<std::string> { "backend_unavailable" }
                    : std::vector<std::string> { backend.unavailable_reason },
    };
  }
  if (validation.normalized.required_isolation &&
      *validation.normalized.required_isolation != backend.isolation) {
    return {
      .error = sandbox_compile_error::unsupported_isolation,
      .message = "sandbox backend does not provide the required isolation level",
      .blockers = { "unsupported_isolation" },
    };
  }
  auto blockers =
    enforcement_blockers(effective_requirements(validation.normalized), backend.enforcement);
  if (!blockers.empty()) {
    return {
      .error = sandbox_compile_error::unsupported_policy,
      .message = "sandbox backend cannot enforce the requested policy",
      .blockers = std::move(blockers),
    };
  }
  return {
    .plan = std::make_shared<logical_sandbox_plan>(
      backend.name, platform, std::move(validation.normalized), backend.enforcement),
  };
}

} // namespace wuwe::agent::sandbox
