#ifndef WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_SANDBOX_PLAN_MACOS_HPP
#define WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_SANDBOX_PLAN_MACOS_HPP

#ifdef __APPLE__

#include <cstdint>
#include <memory>
#include <string>

#include <wuwe/agent/execution/restricted_process_backend.hpp>

namespace wuwe::agent::execution::detail {

class macos_restricted_process_sandbox_plan final : public sandbox::sandbox_plan {
public:
  static constexpr std::uint32_t current_format_version = 4;

  [[nodiscard]] std::uint32_t format_version() const noexcept {
    return format_version_;
  }
  [[nodiscard]] const restricted_process_backend_config& runtime_config() const noexcept {
    return runtime_config_;
  }
  [[nodiscard]] const std::string& seatbelt_profile() const noexcept {
    return seatbelt_profile_;
  }

private:
  friend sandbox::sandbox_compile_result compile_macos_restricted_process_sandbox_policy(
    const sandbox::sandbox_policy&, const sandbox::sandbox_backend_info&,
    restricted_process_backend_config);

  macos_restricted_process_sandbox_plan(sandbox::sandbox_policy policy,
    sandbox::sandbox_enforcement_contract enforcement, restricted_process_backend_config config,
    std::string profile);

  std::uint32_t format_version_ { current_format_version };
  restricted_process_backend_config runtime_config_;
  std::string seatbelt_profile_;
};

[[nodiscard]] sandbox::sandbox_compile_result compile_macos_restricted_process_sandbox_policy(
  const sandbox::sandbox_policy& policy, const sandbox::sandbox_backend_info& backend,
  restricted_process_backend_config config);

[[nodiscard]] std::shared_ptr<const macos_restricted_process_sandbox_plan>
as_macos_restricted_process_sandbox_plan(
  const std::shared_ptr<const sandbox::sandbox_plan>& plan) noexcept;

} // namespace wuwe::agent::execution::detail

#endif // __APPLE__
#endif
