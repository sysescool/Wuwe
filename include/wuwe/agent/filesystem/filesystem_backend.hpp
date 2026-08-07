#ifndef WUWE_AGENT_FILESYSTEM_FILESYSTEM_BACKEND_HPP
#define WUWE_AGENT_FILESYSTEM_FILESYSTEM_BACKEND_HPP

#include <stop_token>

#include <wuwe/agent/filesystem/filesystem_core.hpp>

namespace wuwe::agent::filesystem {

class filesystem_backend {
public:
  virtual ~filesystem_backend() = default;

  [[nodiscard]] virtual filesystem_result read_text(
    const read_text_request& request, std::stop_token stop_token) = 0;
  [[nodiscard]] virtual filesystem_result file_info(
    const file_info_request& request, std::stop_token stop_token) = 0;
  [[nodiscard]] virtual filesystem_result write_text(
    const write_text_request& request, std::stop_token stop_token) = 0;
  [[nodiscard]] virtual filesystem_result replace_text(
    const replace_text_request& request, std::stop_token stop_token) = 0;
  [[nodiscard]] virtual filesystem_result list_directory(
    const list_directory_request& request, std::stop_token stop_token) = 0;
  [[nodiscard]] virtual filesystem_result glob(
    const glob_request& request, std::stop_token stop_token) = 0;
  [[nodiscard]] virtual filesystem_result search_text(
    const search_text_request& request, std::stop_token stop_token) = 0;
  [[nodiscard]] virtual filesystem_result create_directory(
    const create_directory_request& request, std::stop_token stop_token) = 0;
  [[nodiscard]] virtual filesystem_result copy_path(
    const transfer_path_request& request, std::stop_token stop_token) = 0;
  [[nodiscard]] virtual filesystem_result move_path(
    const transfer_path_request& request, std::stop_token stop_token) = 0;
  [[nodiscard]] virtual filesystem_result remove_path(
    const remove_path_request& request, std::stop_token stop_token) = 0;
};

} // namespace wuwe::agent::filesystem

#endif // WUWE_AGENT_FILESYSTEM_FILESYSTEM_BACKEND_HPP
