#ifndef WUWE_AGENT_PROCESS_PROCESS_RUNTIME_HPP
#define WUWE_AGENT_PROCESS_PROCESS_RUNTIME_HPP

#include <atomic>
#include <memory>
#include <stop_token>

#include <wuwe/agent/approval/approval_service.hpp>
#include <wuwe/agent/audit/audit_sink.hpp>
#include <wuwe/agent/process/process_backend.hpp>
#include <wuwe/agent/process/process_policy.hpp>

namespace wuwe::agent::process {

class process_runtime {
public:
  process_runtime(std::unique_ptr<process_backend> backend, process_policy policy,
    audit::audit_sink* audit = nullptr, approval::approval_service* approvals = nullptr);

  process_runtime(const process_runtime&) = delete;
  process_runtime& operator=(const process_runtime&) = delete;
  process_runtime(process_runtime&&) = delete;
  process_runtime& operator=(process_runtime&&) = delete;

  [[nodiscard]] process_result run(process_request request, std::stop_token stop_token = {});
  [[nodiscard]] process_result run_shell(shell_request request, std::stop_token stop_token = {});

  [[nodiscard]] const process_policy& policy() const noexcept;
  [[nodiscard]] const process_backend* backend() const noexcept;

  void audit_tool_rejection(const std::string& tool_name, const std::string& reason,
    const std::map<std::string, std::string>& attributes = {});

private:
  [[nodiscard]] process_result run_impl(
    process_request request, bool shell, std::stop_token stop_token);

  std::unique_ptr<process_backend> backend_;
  process_policy policy_;
  audit::audit_sink* audit_;
  approval::approval_service* approvals_;
  std::atomic_size_t next_process_id_ { 1 };
};

} // namespace wuwe::agent::process

#endif // WUWE_AGENT_PROCESS_PROCESS_RUNTIME_HPP
