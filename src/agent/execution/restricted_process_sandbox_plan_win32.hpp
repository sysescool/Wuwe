#ifndef WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_SANDBOX_PLAN_WIN32_HPP
#define WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_SANDBOX_PLAN_WIN32_HPP

#ifdef _WIN32

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <wuwe/agent/execution/restricted_process_backend.hpp>

namespace wuwe::agent::execution::detail {

enum class restricted_path_access {
  denied,
  readable,
  writable,
  protected_read_only,
};

class windows_restricted_process_sandbox_plan final : public sandbox::sandbox_plan {
public:
  static constexpr std::uint32_t current_format_version = 1;

  [[nodiscard]] std::uint32_t format_version() const noexcept {
    return format_version_;
  }

  [[nodiscard]] std::uint64_t plan_id() const noexcept {
    return plan_id_;
  }

  [[nodiscard]] const restricted_process_backend_config& runtime_config() const noexcept {
    return runtime_config_;
  }

  [[nodiscard]] restricted_path_access access_for(const std::filesystem::path& path) const;

  [[nodiscard]] std::size_t constrain_process_count(std::size_t requested) const noexcept;
  [[nodiscard]] std::uint64_t constrain_memory_bytes(std::uint64_t requested) const noexcept;
  [[nodiscard]] std::chrono::milliseconds constrain_cpu_time(
    std::chrono::milliseconds requested) const noexcept;

private:
  friend sandbox::sandbox_compile_result compile_windows_restricted_process_sandbox_policy(
    const sandbox::sandbox_policy& policy, const sandbox::sandbox_backend_info& backend,
    restricted_process_backend_config config);

  windows_restricted_process_sandbox_plan(std::uint64_t plan_id, sandbox::sandbox_policy policy,
    sandbox::sandbox_enforcement_contract enforcement,
    restricted_process_backend_config runtime_config);

  std::uint32_t format_version_ { current_format_version };
  std::uint64_t plan_id_ {};
  restricted_process_backend_config runtime_config_;
};

[[nodiscard]] sandbox::sandbox_compile_result compile_windows_restricted_process_sandbox_policy(
  const sandbox::sandbox_policy& policy, const sandbox::sandbox_backend_info& backend,
  restricted_process_backend_config config);

[[nodiscard]] std::shared_ptr<const windows_restricted_process_sandbox_plan>
as_windows_restricted_process_sandbox_plan(
  const std::shared_ptr<const sandbox::sandbox_plan>& plan) noexcept;

} // namespace wuwe::agent::execution::detail

#endif // _WIN32

#endif // WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_SANDBOX_PLAN_WIN32_HPP
