#ifndef WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_PATH_WIN32_HPP
#define WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_PATH_WIN32_HPP

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wuwe::agent::execution::detail {

enum class restricted_windows_path_status {
  ok,
  empty_path,
  absolute_path_failed,
  path_not_found,
  open_failed,
  reparse_point_not_allowed,
  unexpected_file_type,
  hard_link_not_allowed,
  create_directory_failed,
  create_file_failed,
  read_failed,
  write_failed,
};

enum class restricted_windows_path_kind {
  any,
  file,
  directory,
};

struct restricted_windows_path_result {
  restricted_windows_path_status status { restricted_windows_path_status::ok };
  DWORD win32_error { ERROR_SUCCESS };
  std::filesystem::path path;
  std::string detail;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == restricted_windows_path_status::ok;
  }
};

class restricted_windows_locked_path {
public:
  restricted_windows_locked_path() = default;
  ~restricted_windows_locked_path();

  restricted_windows_locked_path(const restricted_windows_locked_path&) = delete;
  restricted_windows_locked_path& operator=(const restricted_windows_locked_path&) = delete;

  restricted_windows_locked_path(restricted_windows_locked_path&& other) noexcept;
  restricted_windows_locked_path& operator=(restricted_windows_locked_path&& other) noexcept;

  [[nodiscard]] HANDLE leaf_handle() const noexcept;
  [[nodiscard]] const std::filesystem::path& path() const noexcept;
  [[nodiscard]] bool is_directory() const noexcept;
  [[nodiscard]] bool valid() const noexcept;

  void reset() noexcept;

private:
  friend struct restricted_windows_path_builder;
  friend restricted_windows_path_result lock_restricted_windows_path(const std::filesystem::path&,
    DWORD, restricted_windows_path_kind, bool, restricted_windows_locked_path&, DWORD);
  friend restricted_windows_path_result create_restricted_windows_directories(
    const std::filesystem::path&, restricted_windows_locked_path&);
  friend restricted_windows_path_result create_restricted_windows_file(
    const std::filesystem::path&, DWORD, DWORD, std::string_view, restricted_windows_locked_path&);

  std::filesystem::path path_;
  std::vector<HANDLE> handles_;
  bool directory_ { false };
};

[[nodiscard]] const char* to_string(restricted_windows_path_status status) noexcept;

[[nodiscard]] restricted_windows_path_result lock_restricted_windows_path(
  const std::filesystem::path& path, DWORD leaf_access, restricted_windows_path_kind expected_kind,
  bool reject_hard_links, restricted_windows_locked_path& locked,
  DWORD leaf_share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE);

[[nodiscard]] restricted_windows_path_result create_restricted_windows_directories(
  const std::filesystem::path& path, restricted_windows_locked_path& locked);

[[nodiscard]] restricted_windows_path_result create_restricted_windows_file(
  const std::filesystem::path& path, DWORD access, DWORD share_mode, std::string_view contents,
  restricted_windows_locked_path& locked);

[[nodiscard]] restricted_windows_path_result copy_restricted_windows_file(
  const std::filesystem::path& source, const std::filesystem::path& destination);

} // namespace wuwe::agent::execution::detail

#endif // _WIN32

#endif // WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_PATH_WIN32_HPP
