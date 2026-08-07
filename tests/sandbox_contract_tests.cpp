#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <wuwe/agent/execution/restricted_process_backend.hpp>
#include <wuwe/agent/sandbox/sandbox_module.hpp>

namespace {

using namespace wuwe::agent;

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

sandbox::sandbox_enforcement_contract fully_enforced_contract() {
  return {
    .shell_execution = sandbox::enforcement_level::enforced,
    .timeout = sandbox::enforcement_level::enforced,
    .cancellation = sandbox::enforcement_level::enforced,
    .stdout_limit = sandbox::enforcement_level::enforced,
    .stderr_limit = sandbox::enforcement_level::enforced,
    .environment_allowlist = sandbox::enforcement_level::enforced,
    .working_directory = sandbox::enforcement_level::enforced,
    .process_tree_cleanup = sandbox::enforcement_level::enforced,
    .process_count_limit = sandbox::enforcement_level::enforced,
    .cpu_time_limit = sandbox::enforcement_level::enforced,
    .memory_limit = sandbox::enforcement_level::enforced,
    .filesystem_read_deny = sandbox::enforcement_level::enforced,
    .filesystem_write_deny = sandbox::enforcement_level::enforced,
    .network_deny = sandbox::enforcement_level::enforced,
    .network_filter = sandbox::enforcement_level::enforced,
  };
}

sandbox::sandbox_policy representative_policy() {
  const auto root = std::filesystem::temp_directory_path() / "wuwe-sandbox-contract";
  return {
    .name = "project-edit",
    .required_isolation = sandbox::isolation_level::restricted_process,
    .filesystem = {
      .read_access = sandbox::sandbox_filesystem_read_access::restricted,
      .readable_roots = { root, root },
      .writable_roots = { root / "workspace" },
      .protected_read_only_paths = { root / "workspace" / ".git" },
      .denied_paths = { root / "workspace" / ".env" },
    },
    .network = {
      .mode = sandbox::sandbox_network_mode::filtered,
      .default_action = sandbox::sandbox_network_action::deny,
      .rules = {
        { .host_pattern = "api.example.com",
          .action = sandbox::sandbox_network_action::allow,
          .port = 443 },
        { .host_pattern = "**.invalid.example",
          .action = sandbox::sandbox_network_action::deny },
      },
    },
    .environment = {
      .variables = { { "LANG", "C.UTF-8" } },
    },
    .resources = {
      .max_process_count = 8,
      .max_memory_bytes = 256 * 1024 * 1024,
      .max_cpu_time = std::chrono::seconds(3),
    },
    .required_enforcement = {
      .timeout = true,
      .cancellation = true,
      .stdout_limit = true,
      .stderr_limit = true,
      .working_directory = true,
    },
    .metadata = { { "owner", "runtime" } },
  };
}

void policy_validation_is_explicit_and_deterministic() {
  auto policy = representative_policy();
  const auto validated = sandbox::validate_sandbox_policy(policy);
  require(validated && validated.normalized.filesystem.readable_roots.size() == 1,
    "valid policy paths should be normalized and duplicate-free");

  policy.filesystem.readable_roots.push_back("relative-root");
  policy.environment.variables["INVALID=NAME"] = "value";
  policy.network.mode = sandbox::sandbox_network_mode::denied;
  policy.network.default_action = sandbox::sandbox_network_action::allow;
  policy.resources.max_process_count = 0;
  const auto invalid = sandbox::validate_sandbox_policy(std::move(policy));
  require(!invalid && invalid.issues.size() >= 4,
    "ambiguous or platform-dependent policies must fail validation explicitly");
}

void policy_codec_round_trips_the_portable_contract() {
  const auto validated = sandbox::validate_sandbox_policy(representative_policy());
  require(static_cast<bool>(validated), "representative sandbox policy must be valid");
  const auto encoded = sandbox::sandbox_policy_to_json(validated.normalized);
  const auto restored = sandbox::sandbox_policy_from_json(encoded);
  require(restored.name == "project-edit" && restored.required_isolation &&
            *restored.required_isolation == sandbox::isolation_level::restricted_process &&
            restored.filesystem.readable_roots.size() == 1 &&
            restored.filesystem.protected_read_only_paths.size() == 1 &&
            restored.network.mode == sandbox::sandbox_network_mode::filtered &&
            restored.network.default_action == sandbox::sandbox_network_action::deny &&
            restored.network.rules.size() == 2 && restored.network.rules.front().port == 443 &&
            restored.resources.max_process_count == 8 &&
            restored.required_enforcement.working_directory &&
            restored.metadata.at("owner") == "runtime",
    "sandbox policy JSON must preserve every portable security decision");

  auto unsupported = encoded;
  unsupported["schema_version"] = 99;
  bool rejected = false;
  try {
    (void)sandbox::sandbox_policy_from_json(unsupported);
  }
  catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "unknown sandbox policy schemas must fail explicitly");

  auto misspelled = encoded;
  misspelled["network"]["mdoe"] = "unrestricted";
  rejected = false;
  try {
    (void)sandbox::sandbox_policy_from_json(misspelled);
  }
  catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "unknown security policy fields must not be ignored silently");
}

void compilation_fails_closed_on_missing_enforcement() {
  auto policy = representative_policy();
  sandbox::sandbox_backend_info backend {
    .name = "test-backend",
    .isolation = sandbox::isolation_level::restricted_process,
    .available = true,
    .enforcement = fully_enforced_contract(),
  };
  const auto compiled =
    sandbox::compile_sandbox_policy(policy, backend, sandbox::sandbox_platform::host_linux);
  require(compiled && compiled.plan->backend_name() == "test-backend" &&
            compiled.plan->platform() == sandbox::sandbox_platform::host_linux &&
            compiled.plan->policy().filesystem.readable_roots.size() == 1 &&
            compiled.plan->enforcement().network_filter == sandbox::enforcement_level::enforced,
    "successful compilation must retain normalized policy and enforcement evidence");

  backend.enforcement.network_filter = sandbox::enforcement_level::not_enforced;
  const auto missing_filter = sandbox::compile_sandbox_policy(policy, backend);
  require(!missing_filter &&
            missing_filter.error == sandbox::sandbox_compile_error::unsupported_policy &&
            std::find(missing_filter.blockers.begin(),
              missing_filter.blockers.end(),
              "network_filter_not_enforced") != missing_filter.blockers.end(),
    "filtered networking must never be represented by a deny-only backend");

  backend.available = false;
  backend.unavailable_reason = "missing-platform-helper";
  const auto unavailable = sandbox::compile_sandbox_policy(policy, backend);
  require(!unavailable &&
            unavailable.error == sandbox::sandbox_compile_error::backend_unavailable &&
            unavailable.blockers == std::vector<std::string> { "missing-platform-helper" },
    "an unavailable backend must fail before a launchable plan exists");
}

void restricted_process_uses_the_portable_compiler_without_changing_availability() {
  execution::restricted_process_backend_config config;
  const auto policy = execution::restricted_process_sandbox_policy(config);
  require(policy.name == "restricted_process" && !policy.environment.inherit_parent &&
            policy.network.mode == sandbox::sandbox_network_mode::denied &&
            policy.required_enforcement.filesystem_read_deny &&
            policy.required_enforcement.network_deny,
    "the legacy restricted configuration must map to an explicit portable policy");

  auto compiler = execution::make_restricted_process_sandbox_backend(config);
  require(compiler && compiler->platform() == sandbox::current_sandbox_platform(),
    "the compatibility adapter must expose its actual host platform");
  const auto capabilities = compiler->probe();
  require(capabilities.platform == sandbox::current_sandbox_platform() &&
            capabilities.backend.name == "restricted_process",
    "backend probing must return structured host and enforcement capabilities");
  const auto compiled = compiler->compile(policy);
#ifdef _WIN32
  require(compiled && compiled.plan->backend_name() == "restricted_process",
    "the current Windows restricted backend must compile its existing strong policy");
  require(compiled.plan->metadata().at("plan_format") == "windows_restricted_process" &&
            compiled.plan->metadata().at("plan_format_version") == "1" &&
            compiled.plan->metadata().at("filesystem_platform_defaults") ==
              "windows_appcontainer_intrinsic",
    "Windows compilation must produce a versioned native plan");

  auto without_platform_defaults = policy;
  without_platform_defaults.filesystem.include_platform_defaults = false;
  const auto platform_defaults_compile = compiler->compile(without_platform_defaults);
  require(!platform_defaults_compile &&
            std::find(platform_defaults_compile.blockers.begin(),
              platform_defaults_compile.blockers.end(),
              "filesystem_platform_defaults_required") != platform_defaults_compile.blockers.end(),
    "Windows must fail closed when intrinsic AppContainer filesystem defaults are forbidden");

  auto created = execution::create_restricted_process_backend(compiled.plan);
  require(created && created.backend->info().available,
    "the execution backend must accept its native compiled plan");

  const auto logical = sandbox::compile_sandbox_policy(
    policy, compiler->info(), sandbox::sandbox_platform::host_windows);
  require(static_cast<bool>(logical), "the foreign-plan probe requires a logical plan");
  const auto foreign = execution::create_restricted_process_backend(logical.plan);
  require(
    !foreign &&
      foreign.error == execution::restricted_process_backend_creation_error::incompatible_plan &&
      std::find(foreign.blockers.begin(), foreign.blockers.end(), "foreign_sandbox_plan") !=
        foreign.blockers.end(),
    "the execution backend must reject plans produced outside its native compiler");

  auto unsupported_network = policy;
  unsupported_network.network.mode = sandbox::sandbox_network_mode::unrestricted;
  unsupported_network.required_enforcement.network_deny = false;
  const auto network_compile = compiler->compile(unsupported_network);
  require(!network_compile && std::find(network_compile.blockers.begin(),
                                network_compile.blockers.end(),
                                "network_mode_unsupported") != network_compile.blockers.end(),
    "Windows must reject network semantics it cannot preserve exactly");

  auto missing_root_policy = policy;
  missing_root_policy.filesystem.readable_roots = { std::filesystem::temp_directory_path() /
                                                    "wuwe-sandbox-missing-root" };
  const auto missing_root = compiler->compile(missing_root_policy);
  require(!missing_root && std::any_of(missing_root.blockers.begin(),
                             missing_root.blockers.end(),
                             [](const std::string& blocker) {
                               return blocker.find("path_unavailable") != std::string::npos;
                             }),
    "Windows compilation must reject policy roots that cannot be bound safely");

  auto colliding_environment = policy;
  colliding_environment.environment.variables = {
    { "Path", "first" },
    { "PATH", "second" },
  };
  const auto environment_collision = compiler->compile(colliding_environment);
  require(!environment_collision && std::find(environment_collision.blockers.begin(),
                                      environment_collision.blockers.end(),
                                      "environment_case_insensitive_name_collision") !=
                                      environment_collision.blockers.end(),
    "Windows compilation must reject case-insensitive environment name collisions");

  auto missing_python_config = config;
  missing_python_config.python_interpreter =
    std::filesystem::temp_directory_path() / "wuwe-missing-python.exe";
  const auto missing_python =
    execution::make_restricted_process_sandbox_backend(missing_python_config)
      ->compile(execution::restricted_process_sandbox_policy(missing_python_config));
  require(!missing_python && std::find(missing_python.blockers.begin(),
                               missing_python.blockers.end(),
                               "python_interpreter_unavailable") != missing_python.blockers.end(),
    "runtime prerequisites must be part of authoritative Windows compilation");

  config.use_job_object = false;
  const auto weak_job = execution::make_restricted_process_sandbox_backend(config)->compile(
    execution::restricted_process_sandbox_policy(config));
  require(!weak_job && std::find(weak_job.blockers.begin(),
                         weak_job.blockers.end(),
                         "process_tree_cleanup_not_enforced") != weak_job.blockers.end(),
    "disabling Windows job enforcement must remain an explicit availability blocker");
#else
  require(!compiled && compiled.error == sandbox::sandbox_compile_error::backend_unavailable &&
            std::find(compiled.blockers.begin(),
              compiled.blockers.end(),
              "restricted_process_unsupported_platform") != compiled.blockers.end(),
    "non-Windows restricted execution must remain unavailable in phase one");
#endif
}

template<typename Test>
void run(const char* name, Test&& test) {
  test();
  std::cout << "[PASS] " << name << '\n';
}

} // namespace

int main() {
  try {
    run("sandbox policy validation", policy_validation_is_explicit_and_deterministic);
    run("sandbox policy codec", policy_codec_round_trips_the_portable_contract);
    run("sandbox policy compilation", compilation_fails_closed_on_missing_enforcement);
    run("restricted process adapter",
      restricted_process_uses_the_portable_compiler_without_changing_availability);
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "[FAIL] " << error.what() << '\n';
    return 1;
  }
}
