#ifndef WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_ACL_WIN32_HPP
#define WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_ACL_WIN32_HPP

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace wuwe::agent::execution::detail {

enum class restricted_acl_grant_status {
  ok,
  invalid_sid,
  empty_path,
  path_not_found,
  current_user_sid_failed,
  well_known_sid_failed,
  build_acl_failed,
  set_dacl_failed,
  iterate_failed,
  reparse_point_not_allowed,
  hard_link_not_allowed,
  revoke_failed,
};

class restricted_acl_lease;

struct restricted_acl_grant_request {
  std::filesystem::path path;
  PSID sid {};
  DWORD directory_access {};
  DWORD file_access {};
  restricted_acl_lease* lease {};
};

struct restricted_acl_grant_result {
  restricted_acl_grant_status status { restricted_acl_grant_status::ok };
  DWORD win32_error { ERROR_SUCCESS };
  std::string detail;
  std::size_t directories_granted {};
  std::size_t files_granted {};
};

class restricted_acl_lease {
public:
  restricted_acl_lease() = default;
  ~restricted_acl_lease();

  restricted_acl_lease(const restricted_acl_lease&) = delete;
  restricted_acl_lease& operator=(const restricted_acl_lease&) = delete;

  restricted_acl_lease(restricted_acl_lease&& other) noexcept;
  restricted_acl_lease& operator=(restricted_acl_lease&& other) noexcept;

  [[nodiscard]] restricted_acl_grant_result restore() noexcept;
  void track(const std::filesystem::path& path, PACL dacl, SECURITY_DESCRIPTOR_CONTROL control);
  void track_scope(const std::filesystem::path& path, PSID sid);

private:
  friend restricted_acl_grant_result grant_restricted_tree_access(
    const restricted_acl_grant_request& request);
  friend restricted_acl_grant_result protect_restricted_tree_read_only(
    const restricted_acl_grant_request& request);
  friend restricted_acl_grant_result deny_restricted_tree_access(
    const restricted_acl_grant_request& request);

  struct snapshot {
    std::filesystem::path path;
    std::vector<DWORD> dacl;
    bool null_dacl { false };
    bool protected_dacl { false };
  };

  std::vector<snapshot> snapshots_;
  std::vector<std::filesystem::path> scope_roots_;
  std::vector<DWORD> sid_;
};

[[nodiscard]] const char* to_string(restricted_acl_grant_status status) noexcept;

[[nodiscard]] restricted_acl_grant_result grant_restricted_file_access(
  const std::filesystem::path& path, PSID sid, DWORD access_permissions);

[[nodiscard]] restricted_acl_grant_result grant_restricted_directory_access(
  const std::filesystem::path& path, PSID sid, DWORD access_permissions);

[[nodiscard]] restricted_acl_grant_result grant_restricted_tree_access(
  const restricted_acl_grant_request& request);

[[nodiscard]] restricted_acl_grant_result protect_restricted_tree_read_only(
  const restricted_acl_grant_request& request);

[[nodiscard]] restricted_acl_grant_result deny_restricted_tree_access(
  const restricted_acl_grant_request& request);

} // namespace wuwe::agent::execution::detail

#endif // _WIN32

#endif // WUWE_AGENT_EXECUTION_RESTRICTED_PROCESS_ACL_WIN32_HPP
