#ifndef WUWE_AGENT_FILESYSTEM_FILESYSTEM_RUNTIME_HPP
#define WUWE_AGENT_FILESYSTEM_FILESYSTEM_RUNTIME_HPP

#include <atomic>
#include <memory>
#include <stop_token>

#include <wuwe/agent/approval/approval_service.hpp>
#include <wuwe/agent/audit/audit_sink.hpp>
#include <wuwe/agent/filesystem/filesystem_backend.hpp>
#include <wuwe/agent/filesystem/filesystem_policy.hpp>

namespace wuwe::agent::filesystem {

class filesystem_runtime {
public:
  filesystem_runtime(
    std::unique_ptr<filesystem_backend> backend,
    filesystem_policy policy,
    audit::audit_sink* audit = nullptr,
    approval::approval_service* approvals = nullptr);

  filesystem_runtime(const filesystem_runtime&) = delete;
  filesystem_runtime& operator=(const filesystem_runtime&) = delete;
  filesystem_runtime(filesystem_runtime&&) = delete;
  filesystem_runtime& operator=(filesystem_runtime&&) = delete;

  [[nodiscard]] filesystem_result read_text(
    read_text_request request,
    std::stop_token stop_token = {});
  [[nodiscard]] filesystem_result file_info(
    file_info_request request,
    std::stop_token stop_token = {});
  [[nodiscard]] filesystem_result write_text(
    write_text_request request,
    std::stop_token stop_token = {});
  [[nodiscard]] filesystem_result replace_text(
    replace_text_request request,
    std::stop_token stop_token = {});
  [[nodiscard]] filesystem_result list_directory(
    list_directory_request request,
    std::stop_token stop_token = {});
  [[nodiscard]] filesystem_result glob(
    glob_request request,
    std::stop_token stop_token = {});
  [[nodiscard]] filesystem_result search_text(
    search_text_request request,
    std::stop_token stop_token = {});
  [[nodiscard]] filesystem_result create_directory(
    create_directory_request request,
    std::stop_token stop_token = {});
  [[nodiscard]] filesystem_result copy_path(
    transfer_path_request request,
    std::stop_token stop_token = {});
  [[nodiscard]] filesystem_result move_path(
    transfer_path_request request,
    std::stop_token stop_token = {});
  [[nodiscard]] filesystem_result remove_path(
    remove_path_request request,
    std::stop_token stop_token = {});

  [[nodiscard]] const filesystem_policy& policy() const noexcept;
  [[nodiscard]] const filesystem_backend* backend() const noexcept;

  void audit_tool_rejection(
    const std::string& tool_name,
    const std::string& reason,
    const std::map<std::string, std::string>& attributes = {});

private:
  struct operation_context;

  [[nodiscard]] std::unique_ptr<operation_context> begin_operation(
    std::string operation,
    std::vector<std::filesystem::path> resources,
    bool write,
    bool approval_required,
    std::map<std::string, std::string> metadata);
  [[nodiscard]] std::optional<std::filesystem::path> resolve_path(
    const std::filesystem::path& path,
    operation_context& context) const;
  [[nodiscard]] bool authorize(
    operation_context& context,
    std::stop_token stop_token);
  [[nodiscard]] filesystem_result finish(
    operation_context& context,
    filesystem_result result) const;

  std::unique_ptr<filesystem_backend> backend_;
  filesystem_policy policy_;
  audit::audit_sink* audit_;
  approval::approval_service* approvals_;
  std::atomic_size_t next_operation_id_ { 1 };
};

} // namespace wuwe::agent::filesystem

#endif // WUWE_AGENT_FILESYSTEM_FILESYSTEM_RUNTIME_HPP
