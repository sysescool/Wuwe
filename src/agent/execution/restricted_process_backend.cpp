#include <wuwe/agent/execution/restricted_process_backend.hpp>

#include <utility>

#ifdef _WIN32
#include "restricted_process_execution_plan_win32.hpp"
#include "restricted_process_sandbox_plan_win32.hpp"
#endif

namespace wuwe::agent::execution {
namespace {

#ifdef _WIN32
class restricted_process_backend final : public execution_backend {
public:
  explicit restricted_process_backend(
    std::shared_ptr<const detail::windows_restricted_process_sandbox_plan> plan)
      : plan_(std::move(plan)) {
  }

  [[nodiscard]] sandbox::sandbox_backend_info info() const override {
    auto descriptor = restricted_process_backend_descriptor();
    descriptor.available = true;
    descriptor.enforcement = plan_->enforcement();
    descriptor.unavailable_reason.clear();
    return descriptor;
  }

  [[nodiscard]] execution_result run(
    const execution_request& request, std::stop_token stop_token) override {
    return detail::run_restricted_execution_plan(plan_, request, stop_token);
  }

private:
  std::shared_ptr<const detail::windows_restricted_process_sandbox_plan> plan_;
};
#endif

class restricted_process_policy_backend final : public sandbox::sandbox_backend {
public:
  explicit restricted_process_policy_backend(restricted_process_backend_config config)
      : config_(std::move(config)) {
  }

  [[nodiscard]] sandbox::sandbox_backend_info info() const override {
    auto descriptor = restricted_process_backend_descriptor();
    descriptor.enforcement = restricted_process_backend_configured_contract(config_);
#ifdef _WIN32
    descriptor.available = true;
    descriptor.unavailable_reason.clear();
#else
    descriptor.available = false;
    descriptor.unavailable_reason = "restricted_process_unsupported_platform";
#endif
    return descriptor;
  }

  [[nodiscard]] sandbox::sandbox_platform platform() const noexcept override {
    return sandbox::current_sandbox_platform();
  }

  [[nodiscard]] sandbox::sandbox_compile_result compile(
    const sandbox::sandbox_policy& policy) const override {
#ifdef _WIN32
    return detail::compile_windows_restricted_process_sandbox_policy(policy, info(), config_);
#else
    return sandbox::compile_sandbox_policy(policy, info(), platform());
#endif
  }

  [[nodiscard]] sandbox::sandbox_host_capabilities probe() const override {
    auto capabilities = sandbox::sandbox_host_capabilities {
      .platform = platform(),
      .backend = info(),
    };
    const auto compiled = compile(restricted_process_sandbox_policy(config_));
    if (!compiled) {
      capabilities.backend.available = false;
      capabilities.backend.unavailable_reason = compiled.message;
      capabilities.blockers = compiled.blockers;
    }
    return capabilities;
  }

private:
  restricted_process_backend_config config_;
};

} // namespace

sandbox::sandbox_enforcement_contract restricted_process_backend_planned_contract() {
  return {
    .shell_execution = sandbox::enforcement_level::planned,
    .timeout = sandbox::enforcement_level::planned,
    .cancellation = sandbox::enforcement_level::planned,
    .stdout_limit = sandbox::enforcement_level::planned,
    .stderr_limit = sandbox::enforcement_level::planned,
    .environment_allowlist = sandbox::enforcement_level::planned,
    .working_directory = sandbox::enforcement_level::planned,
    .process_tree_cleanup = sandbox::enforcement_level::planned,
    .process_count_limit = sandbox::enforcement_level::planned,
    .cpu_time_limit = sandbox::enforcement_level::planned,
    .memory_limit = sandbox::enforcement_level::planned,
    .filesystem_read_deny = sandbox::enforcement_level::planned,
    .filesystem_write_deny = sandbox::enforcement_level::planned,
    .network_deny = sandbox::enforcement_level::planned,
    .network_filter = sandbox::enforcement_level::planned,
  };
}

sandbox::sandbox_enforcement_contract restricted_process_backend_configured_contract(
  const restricted_process_backend_config& config) {
#ifdef _WIN32
  const auto job_enforcement = config.use_job_object ? sandbox::enforcement_level::enforced
                                                     : sandbox::enforcement_level::not_enforced;
  return {
    .shell_execution = sandbox::enforcement_level::enforced,
    .timeout = sandbox::enforcement_level::enforced,
    .cancellation = sandbox::enforcement_level::enforced,
    .stdout_limit = sandbox::enforcement_level::enforced,
    .stderr_limit = sandbox::enforcement_level::enforced,
    .environment_allowlist = sandbox::enforcement_level::enforced,
    .working_directory = sandbox::enforcement_level::enforced,
    .process_tree_cleanup = job_enforcement,
    .process_count_limit = job_enforcement,
    .cpu_time_limit = job_enforcement,
    .memory_limit = job_enforcement,
    .filesystem_read_deny = sandbox::enforcement_level::enforced,
    .filesystem_write_deny = sandbox::enforcement_level::enforced,
    .network_deny = config.deny_network ? sandbox::enforcement_level::enforced
                                        : sandbox::enforcement_level::not_enforced,
  };
#else
  (void)config;
  auto contract = restricted_process_backend_planned_contract();
  contract.process_tree_cleanup = sandbox::enforcement_level::not_enforced;
  contract.process_count_limit = sandbox::enforcement_level::not_enforced;
  contract.cpu_time_limit = sandbox::enforcement_level::not_enforced;
  contract.memory_limit = sandbox::enforcement_level::not_enforced;
  contract.filesystem_read_deny = sandbox::enforcement_level::not_enforced;
  contract.filesystem_write_deny = sandbox::enforcement_level::not_enforced;
  contract.network_deny = sandbox::enforcement_level::not_enforced;
  return contract;
#endif
}

restricted_process_backend_availability evaluate_restricted_process_backend_availability(
  const restricted_process_backend_config& config) {
  return evaluate_restricted_process_backend_availability(
    config, restricted_process_backend_registration::descriptor_only);
}

restricted_process_backend_availability evaluate_restricted_process_backend_availability(
  const restricted_process_backend_config& config,
  restricted_process_backend_registration registration) {
  auto backend = make_restricted_process_sandbox_backend(config);
  const auto info = backend->info();
  const auto compiled = backend->compile(restricted_process_sandbox_policy(config));
  restricted_process_backend_availability result { .contract = info.enforcement };
  if (!compiled) {
    result.blockers = compiled.blockers;
  }
  if (compiled && registration != restricted_process_backend_registration::registered_factory) {
    result.blockers.emplace_back("restricted_process_backend_not_registered");
  }
  result.available = compiled && result.blockers.empty();
  return result;
}

const char* to_string(restricted_process_runtime_staging staging) noexcept {
  switch (staging) {
    case restricted_process_runtime_staging::copy_minimal_python_runtime:
      return "copy_minimal_python_runtime";
  }
  return "unknown";
}

const char* to_string(restricted_process_backend_creation_error error) noexcept {
  switch (error) {
    case restricted_process_backend_creation_error::none:
      return "none";
    case restricted_process_backend_creation_error::compile_failed:
      return "compile_failed";
    case restricted_process_backend_creation_error::null_plan:
      return "null_plan";
    case restricted_process_backend_creation_error::incompatible_plan:
      return "incompatible_plan";
    case restricted_process_backend_creation_error::stale_plan:
      return "stale_plan";
  }
  return "incompatible_plan";
}

sandbox::sandbox_backend_info restricted_process_backend_descriptor() {
  return {
    .name = "restricted_process",
    .isolation = sandbox::isolation_level::restricted_process,
    .available = false,
#ifdef _WIN32
    .unavailable_reason =
      "restricted_process requires successful policy compilation and explicit registration",
#else
    .unavailable_reason =
      "restricted_process is unavailable on this platform",
#endif
    .features = {
      sandbox::sandbox_feature::environment_allowlist,
      sandbox::sandbox_feature::working_directory,
      sandbox::sandbox_feature::stdout_capture,
      sandbox::sandbox_feature::stderr_capture,
      sandbox::sandbox_feature::timeout,
      sandbox::sandbox_feature::cancellation,
      sandbox::sandbox_feature::filesystem_read_restriction,
      sandbox::sandbox_feature::filesystem_write_restriction,
      sandbox::sandbox_feature::network_restriction,
    },
    .enforcement = restricted_process_backend_planned_contract(),
  };
}

sandbox::sandbox_policy restricted_process_sandbox_policy(
  const restricted_process_backend_config& config) {
  sandbox::sandbox_policy policy {
    .name = "restricted_process",
    .required_isolation = sandbox::isolation_level::restricted_process,
    .filesystem = {
      .read_access = sandbox::sandbox_filesystem_read_access::restricted,
      .readable_roots = config.readable_roots,
      .writable_roots = config.writable_roots,
    },
    .network = {
      .mode = config.deny_network ? sandbox::sandbox_network_mode::denied
                                  : sandbox::sandbox_network_mode::unrestricted,
    },
    .environment = {
      .inherit_parent = config.inherit_parent_environment,
      .variables = config.base_environment,
    },
    .resources = {
      .cleanup_process_tree = true,
    },
    .required_enforcement = {
      .shell_execution = true,
      .timeout = true,
      .cancellation = true,
      .stdout_limit = true,
      .stderr_limit = true,
      .environment_allowlist = !config.inherit_parent_environment,
      .working_directory = true,
      .process_tree_cleanup = true,
      .process_count_limit = true,
      .cpu_time_limit = true,
      .memory_limit = true,
      .filesystem_read_deny = true,
      .filesystem_write_deny = true,
      .network_deny = true,
    },
  };
  policy.metadata["adapter"] = "restricted_process_backend_config";
  policy.metadata["runtime_staging"] = to_string(config.runtime_staging);
  return policy;
}

std::unique_ptr<sandbox::sandbox_backend> make_restricted_process_sandbox_backend(
  restricted_process_backend_config config) {
  return std::make_unique<restricted_process_policy_backend>(std::move(config));
}

restricted_process_backend_creation create_restricted_process_backend(
  std::shared_ptr<const sandbox::sandbox_plan> plan) {
  if (!plan) {
    return {
      .error = restricted_process_backend_creation_error::null_plan,
      .message = "restricted_process requires a compiled sandbox plan",
      .blockers = { "null_sandbox_plan" },
    };
  }
#ifdef _WIN32
  auto native = detail::as_windows_restricted_process_sandbox_plan(plan);
  if (!native || native->platform() != sandbox::sandbox_platform::host_windows ||
      native->backend_name() != "restricted_process") {
    return {
      .error = restricted_process_backend_creation_error::incompatible_plan,
      .message = "sandbox plan was not produced by the Windows restricted_process compiler",
      .blockers = { "foreign_sandbox_plan" },
    };
  }
  if (native->format_version() !=
      detail::windows_restricted_process_sandbox_plan::current_format_version) {
    return {
      .error = restricted_process_backend_creation_error::stale_plan,
      .message = "sandbox plan format is stale",
      .blockers = { "stale_sandbox_plan" },
    };
  }
  return {
    .backend = std::make_unique<restricted_process_backend>(std::move(native)),
  };
#else
  return {
    .error = restricted_process_backend_creation_error::incompatible_plan,
    .message = "Windows restricted_process plans cannot execute on this platform",
    .blockers = { "restricted_process_unsupported_platform" },
  };
#endif
}

restricted_process_backend_creation create_restricted_process_backend(
  const sandbox::sandbox_policy& policy, restricted_process_backend_config config) {
  auto compiler = make_restricted_process_sandbox_backend(std::move(config));
  auto compiled = compiler->compile(policy);
  if (!compiled) {
    return {
      .error = restricted_process_backend_creation_error::compile_failed,
      .message = std::move(compiled.message),
      .blockers = std::move(compiled.blockers),
    };
  }
  return create_restricted_process_backend(std::move(compiled.plan));
}

std::unique_ptr<execution_backend> make_restricted_process_backend(
  restricted_process_backend_config config) {
  const auto policy = restricted_process_sandbox_policy(config);
  auto created = create_restricted_process_backend(policy, std::move(config));
  return std::move(created.backend);
}

} // namespace wuwe::agent::execution
