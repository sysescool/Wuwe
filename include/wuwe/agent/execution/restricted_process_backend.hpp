#ifndef WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_BACKEND_HPP
#define WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_BACKEND_HPP

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <wuwe/agent/execution/execution_backend.hpp>
#include <wuwe/agent/sandbox/sandbox_backend.hpp>

namespace wuwe::agent::execution {

enum class restricted_process_runtime_staging {
  copy_minimal_python_runtime,
  use_host_python,
};

struct restricted_process_backend_config {
#ifdef _WIN32
  std::filesystem::path python_interpreter { "python" };
#else
  std::filesystem::path python_interpreter { "python3" };
#endif
  std::filesystem::path fallback_workdir;
  std::filesystem::path runtime_staging_root;
  std::vector<std::filesystem::path> readable_roots;
  std::vector<std::filesystem::path> writable_roots;
  std::map<std::string, std::string> base_environment;
#ifdef _WIN32
  restricted_process_runtime_staging runtime_staging {
    restricted_process_runtime_staging::copy_minimal_python_runtime
  };
#else
  restricted_process_runtime_staging runtime_staging {
    restricted_process_runtime_staging::use_host_python
  };
#endif
  bool deny_network { true };
  bool use_job_object { true };
  bool use_process_group { true };
  std::filesystem::path seatbelt_executable { "/usr/bin/sandbox-exec" };
  bool inherit_parent_environment { false };
  bool cleanup_runtime_staging { true };
  std::chrono::milliseconds python_startup_timeout { 3000 };
};

struct restricted_process_backend_availability {
  bool available { false };
  sandbox::sandbox_enforcement_contract contract;
  std::vector<std::string> blockers;
};

enum class restricted_process_backend_creation_error {
  none,
  compile_failed,
  null_plan,
  incompatible_plan,
  stale_plan,
};

struct restricted_process_backend_creation {
  std::unique_ptr<execution_backend> backend;
  restricted_process_backend_creation_error error {
    restricted_process_backend_creation_error::none
  };
  std::string message;
  std::vector<std::string> blockers;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == restricted_process_backend_creation_error::none && backend != nullptr;
  }
};

enum class restricted_process_backend_registration {
  descriptor_only,
  registered_factory,
};

[[nodiscard]] sandbox::sandbox_enforcement_contract restricted_process_backend_planned_contract();

[[nodiscard]] sandbox::sandbox_enforcement_contract restricted_process_backend_configured_contract(
  const restricted_process_backend_config& config);

[[nodiscard]] restricted_process_backend_availability
evaluate_restricted_process_backend_availability(const restricted_process_backend_config& config);

[[nodiscard]] restricted_process_backend_availability
evaluate_restricted_process_backend_availability(const restricted_process_backend_config& config,
  restricted_process_backend_registration registration);

[[nodiscard]] const char* to_string(restricted_process_runtime_staging staging) noexcept;

[[nodiscard]] const char* to_string(restricted_process_backend_creation_error error) noexcept;

[[nodiscard]] sandbox::sandbox_backend_info restricted_process_backend_descriptor();

[[nodiscard]] sandbox::sandbox_policy restricted_process_sandbox_policy(
  const restricted_process_backend_config& config);

[[nodiscard]] std::unique_ptr<sandbox::sandbox_backend> make_restricted_process_sandbox_backend(
  restricted_process_backend_config config = {});

[[nodiscard]] restricted_process_backend_creation create_restricted_process_backend(
  std::shared_ptr<const sandbox::sandbox_plan> plan);

[[nodiscard]] restricted_process_backend_creation create_restricted_process_backend(
  const sandbox::sandbox_policy& policy, restricted_process_backend_config config = {});

[[nodiscard]] std::unique_ptr<execution_backend> make_restricted_process_backend(
  restricted_process_backend_config config = {});

} // namespace wuwe::agent::execution

#endif // WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_BACKEND_HPP
