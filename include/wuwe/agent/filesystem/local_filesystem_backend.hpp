#ifndef WUWE_AGENT_FILESYSTEM_LOCAL_FILESYSTEM_BACKEND_HPP
#define WUWE_AGENT_FILESYSTEM_LOCAL_FILESYSTEM_BACKEND_HPP

#include <memory>
#include <mutex>

#include <wuwe/agent/filesystem/filesystem_backend.hpp>

namespace wuwe::agent::filesystem {

class local_filesystem_backend final : public filesystem_backend {
public:
  [[nodiscard]] filesystem_result read_text(
    const read_text_request& request, std::stop_token stop_token) override;
  [[nodiscard]] filesystem_result file_info(
    const file_info_request& request, std::stop_token stop_token) override;
  [[nodiscard]] filesystem_result write_text(
    const write_text_request& request, std::stop_token stop_token) override;
  [[nodiscard]] filesystem_result replace_text(
    const replace_text_request& request, std::stop_token stop_token) override;
  [[nodiscard]] filesystem_result list_directory(
    const list_directory_request& request, std::stop_token stop_token) override;
  [[nodiscard]] filesystem_result glob(
    const glob_request& request, std::stop_token stop_token) override;
  [[nodiscard]] filesystem_result search_text(
    const search_text_request& request, std::stop_token stop_token) override;
  [[nodiscard]] filesystem_result create_directory(
    const create_directory_request& request, std::stop_token stop_token) override;
  [[nodiscard]] filesystem_result copy_path(
    const transfer_path_request& request, std::stop_token stop_token) override;
  [[nodiscard]] filesystem_result move_path(
    const transfer_path_request& request, std::stop_token stop_token) override;
  [[nodiscard]] filesystem_result remove_path(
    const remove_path_request& request, std::stop_token stop_token) override;

private:
  // Revision checks and mutations are one process-local critical section.
  // Cross-process coordination still belongs to an OS sandbox or host service.
  std::mutex mutation_mutex_;
};

[[nodiscard]] std::unique_ptr<filesystem_backend> make_local_filesystem_backend();

} // namespace wuwe::agent::filesystem

#endif // WUWE_AGENT_FILESYSTEM_LOCAL_FILESYSTEM_BACKEND_HPP
