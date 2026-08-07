#include "restricted_process_acl_win32.hpp"

#ifdef _WIN32

#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "restricted_process_path_win32.hpp"

namespace wuwe::agent::execution::detail {
namespace {

restricted_acl_grant_result make_acl_result(
  restricted_acl_grant_status status, DWORD win32_error = ERROR_SUCCESS, std::string detail = {}) {
  return {
    .status = status,
    .win32_error = win32_error,
    .detail = std::move(detail),
  };
}

restricted_acl_grant_result acl_result_from_path(const restricted_windows_path_result& result,
  restricted_acl_grant_status fallback = restricted_acl_grant_status::path_not_found) {
  auto status = fallback;
  if (result.status == restricted_windows_path_status::reparse_point_not_allowed) {
    status = restricted_acl_grant_status::reparse_point_not_allowed;
  }
  else if (result.status == restricted_windows_path_status::hard_link_not_allowed) {
    status = restricted_acl_grant_status::hard_link_not_allowed;
  }
  return make_acl_result(status,
    result.win32_error,
    result.path.empty() ? result.detail : result.path.string() + ": " + result.detail);
}

EXPLICIT_ACCESSW explicit_access_for_sid(
  const void* sid, DWORD access_permissions, ACCESS_MODE mode, DWORD inheritance = NO_INHERITANCE) {
  EXPLICIT_ACCESSW entry {};
  entry.grfAccessPermissions = access_permissions;
  entry.grfAccessMode = mode;
  entry.grfInheritance = inheritance;
  entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  entry.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
  entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(const_cast<void*>(sid));
  return entry;
}

enum class acl_update_kind {
  allow,
  protect_read_only,
  deny,
  revoke,
};

constexpr DWORD restricted_read_only_deny_mask = FILE_WRITE_DATA | FILE_APPEND_DATA |
                                                 FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | DELETE |
                                                 FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER;

bool make_well_known_sid(WELL_KNOWN_SID_TYPE type, std::vector<BYTE>& storage) {
  storage.resize(SECURITY_MAX_SID_SIZE);
  DWORD size = static_cast<DWORD>(storage.size());
  if (!CreateWellKnownSid(type, nullptr, storage.data(), &size)) {
    storage.clear();
    return false;
  }
  storage.resize(size);
  return true;
}

bool make_sid_from_string(const wchar_t* text, std::vector<BYTE>& storage) {
  PSID raw_sid = nullptr;
  if (!ConvertStringSidToSidW(text, &raw_sid)) {
    return false;
  }
  const auto size = GetLengthSid(raw_sid);
  storage.resize(size);
  const auto copied = CopySid(size, storage.data(), raw_sid) != FALSE;
  LocalFree(raw_sid);
  if (!copied) {
    storage.clear();
  }
  return copied;
}

PSID sid_for_ace(const ACE_HEADER* header) noexcept {
  if (header == nullptr) {
    return nullptr;
  }
  switch (header->AceType) {
    case ACCESS_ALLOWED_ACE_TYPE:
      return const_cast<DWORD*>(&reinterpret_cast<const ACCESS_ALLOWED_ACE*>(header)->SidStart);
    case ACCESS_DENIED_ACE_TYPE:
      return const_cast<DWORD*>(&reinterpret_cast<const ACCESS_DENIED_ACE*>(header)->SidStart);
    case ACCESS_ALLOWED_CALLBACK_ACE_TYPE:
      return const_cast<DWORD*>(
        &reinterpret_cast<const ACCESS_ALLOWED_CALLBACK_ACE*>(header)->SidStart);
    case ACCESS_DENIED_CALLBACK_ACE_TYPE:
      return const_cast<DWORD*>(
        &reinterpret_cast<const ACCESS_DENIED_CALLBACK_ACE*>(header)->SidStart);
    case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
    case ACCESS_DENIED_OBJECT_ACE_TYPE:
    case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE:
    case ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE: {
      const auto* object = reinterpret_cast<const ACCESS_ALLOWED_OBJECT_ACE*>(header);
      auto* sid = reinterpret_cast<const BYTE*>(&object->SidStart);
      if ((object->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT) == 0) {
        sid -= sizeof(GUID);
      }
      if ((object->Flags & ACE_OBJECT_TYPE_PRESENT) == 0) {
        sid -= sizeof(GUID);
      }
      return const_cast<BYTE*>(sid);
    }
    default:
      return nullptr;
  }
}

bool is_allowed_ace_type(BYTE type) noexcept {
  return type == ACCESS_ALLOWED_ACE_TYPE || type == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
         type == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
         type == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE;
}

bool copy_acl_without_sid(PACL source, PSID sid, std::vector<DWORD>& storage, DWORD& error) {
  if (source == nullptr) {
    error = ERROR_INVALID_ACL;
    return false;
  }
  storage.assign((source->AclSize + sizeof(DWORD) - 1) / sizeof(DWORD), DWORD {});
  auto* filtered = reinterpret_cast<PACL>(storage.data());
  if (!InitializeAcl(filtered, static_cast<DWORD>(storage.size() * sizeof(DWORD)), ACL_REVISION)) {
    error = GetLastError();
    return false;
  }
  ACL_SIZE_INFORMATION information {};
  if (!GetAclInformation(source, &information, sizeof(information), AclSizeInformation)) {
    error = GetLastError();
    return false;
  }
  for (DWORD index = 0; index < information.AceCount; ++index) {
    void* raw_ace = nullptr;
    if (!GetAce(source, index, &raw_ace)) {
      error = GetLastError();
      return false;
    }
    const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
    const auto ace_sid = sid_for_ace(header);
    if (ace_sid != nullptr && IsValidSid(ace_sid) && EqualSid(ace_sid, sid)) {
      continue;
    }
    if (!AddAce(filtered, ACL_REVISION, MAXDWORD, raw_ace, header->AceSize)) {
      error = GetLastError();
      return false;
    }
  }
  return true;
}

bool copy_acl_for_policy_update(PACL source, PSID sandbox_sid, acl_update_kind kind,
  PSID all_application_packages, PSID all_restricted_application_packages,
  std::vector<DWORD>& storage, DWORD& error) {
  if (source == nullptr) {
    error = ERROR_INVALID_ACL;
    return false;
  }
  storage.assign((source->AclSize + sizeof(DWORD) - 1) / sizeof(DWORD), DWORD {});
  auto* filtered = reinterpret_cast<PACL>(storage.data());
  if (!InitializeAcl(filtered, static_cast<DWORD>(storage.size() * sizeof(DWORD)), ACL_REVISION)) {
    error = GetLastError();
    return false;
  }
  ACL_SIZE_INFORMATION information {};
  if (!GetAclInformation(source, &information, sizeof(information), AclSizeInformation)) {
    error = GetLastError();
    return false;
  }
  for (DWORD index = 0; index < information.AceCount; ++index) {
    void* raw_ace = nullptr;
    if (!GetAce(source, index, &raw_ace)) {
      error = GetLastError();
      return false;
    }
    const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
    const auto ace_sid = sid_for_ace(header);
    if (ace_sid != nullptr && IsValidSid(ace_sid) && EqualSid(ace_sid, sandbox_sid)) {
      continue;
    }

    const auto package_allow = is_allowed_ace_type(header->AceType) && ace_sid != nullptr &&
                               IsValidSid(ace_sid) &&
                               (EqualSid(ace_sid, all_application_packages) ||
                                 EqualSid(ace_sid, all_restricted_application_packages));
    if (package_allow && kind == acl_update_kind::deny) {
      continue;
    }
    if (package_allow && kind == acl_update_kind::protect_read_only) {
      std::vector<BYTE> adjusted(header->AceSize);
      std::memcpy(adjusted.data(), raw_ace, header->AceSize);
      auto* allowed = reinterpret_cast<ACCESS_ALLOWED_ACE*>(adjusted.data());
      allowed->Mask &= ~restricted_read_only_deny_mask;
      if (allowed->Mask == 0) {
        continue;
      }
      if (!AddAce(filtered, ACL_REVISION, MAXDWORD, adjusted.data(), header->AceSize)) {
        error = GetLastError();
        return false;
      }
      continue;
    }
    if (!AddAce(filtered, ACL_REVISION, MAXDWORD, raw_ace, header->AceSize)) {
      error = GetLastError();
      return false;
    }
  }
  return true;
}

restricted_acl_grant_result update_path_access(const std::filesystem::path& path, PSID sid,
  DWORD access_permissions, bool directory, acl_update_kind kind,
  restricted_acl_lease* lease = nullptr) {
  if (sid == nullptr || !IsValidSid(sid)) {
    return make_acl_result(restricted_acl_grant_status::invalid_sid);
  }
  if (path.empty()) {
    return make_acl_result(restricted_acl_grant_status::empty_path);
  }

  restricted_windows_locked_path locked;
  const auto lock_result = lock_restricted_windows_path(path,
    READ_CONTROL | WRITE_DAC,
    directory ? restricted_windows_path_kind::directory : restricted_windows_path_kind::file,
    !directory,
    locked);
  if (!lock_result) {
    return acl_result_from_path(lock_result);
  }
  const auto handle = locked.leaf_handle();

  PACL existing_acl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const auto query_error = GetSecurityInfo(handle,
    SE_FILE_OBJECT,
    DACL_SECURITY_INFORMATION,
    nullptr,
    nullptr,
    &existing_acl,
    nullptr,
    &descriptor);
  if (query_error != ERROR_SUCCESS) {
    return make_acl_result(
      restricted_acl_grant_status::build_acl_failed, query_error, path.string());
  }
  struct descriptor_cleanup {
    PSECURITY_DESCRIPTOR descriptor;
    ~descriptor_cleanup() {
      if (descriptor != nullptr) {
        LocalFree(descriptor);
      }
    }
  } cleanup_descriptor { descriptor };

  SECURITY_DESCRIPTOR_CONTROL descriptor_control {};
  DWORD descriptor_revision = 0;
  if (!GetSecurityDescriptorControl(descriptor, &descriptor_control, &descriptor_revision)) {
    return make_acl_result(
      restricted_acl_grant_status::build_acl_failed, GetLastError(), path.string());
  }
  if (lease != nullptr && kind != acl_update_kind::revoke) {
    lease->track(path, existing_acl, descriptor_control);
  }

  constexpr DWORD inherit_to_children = CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
  const auto inheritance = directory ? inherit_to_children : NO_INHERITANCE;
  std::vector<EXPLICIT_ACCESSW> entries;
  std::vector<BYTE> all_application_packages;
  std::vector<BYTE> all_restricted_application_packages;
  if ((kind == acl_update_kind::protect_read_only || kind == acl_update_kind::deny) &&
      (!make_well_known_sid(WinBuiltinAnyPackageSid, all_application_packages) ||
        !make_sid_from_string(L"S-1-15-2-2", all_restricted_application_packages))) {
    return make_acl_result(restricted_acl_grant_status::well_known_sid_failed,
      GetLastError(),
      "application package SID");
  }
  switch (kind) {
    case acl_update_kind::allow:
      entries.push_back(
        explicit_access_for_sid(sid, access_permissions, GRANT_ACCESS, inheritance));
      break;
    case acl_update_kind::protect_read_only:
      entries.push_back(
        explicit_access_for_sid(sid, restricted_read_only_deny_mask, DENY_ACCESS, inheritance));
      entries.push_back(explicit_access_for_sid(
        all_application_packages.data(), restricted_read_only_deny_mask, DENY_ACCESS, inheritance));
      entries.push_back(explicit_access_for_sid(all_restricted_application_packages.data(),
        restricted_read_only_deny_mask,
        DENY_ACCESS,
        inheritance));
      entries.push_back(
        explicit_access_for_sid(sid, access_permissions, GRANT_ACCESS, inheritance));
      break;
    case acl_update_kind::deny:
      entries.push_back(explicit_access_for_sid(sid, FILE_ALL_ACCESS, DENY_ACCESS, inheritance));
      entries.push_back(explicit_access_for_sid(
        all_application_packages.data(), FILE_ALL_ACCESS, DENY_ACCESS, inheritance));
      entries.push_back(explicit_access_for_sid(
        all_restricted_application_packages.data(), FILE_ALL_ACCESS, DENY_ACCESS, inheritance));
      break;
    case acl_update_kind::revoke:
      entries.push_back(explicit_access_for_sid(sid, 0, REVOKE_ACCESS));
      break;
  }

  std::vector<DWORD> filtered_acl_storage;
  DWORD filter_error = ERROR_SUCCESS;
  if (!copy_acl_for_policy_update(existing_acl,
        sid,
        kind,
        all_application_packages.empty() ? sid : all_application_packages.data(),
        all_restricted_application_packages.empty() ? sid
                                                    : all_restricted_application_packages.data(),
        filtered_acl_storage,
        filter_error)) {
    return make_acl_result(restricted_acl_grant_status::build_acl_failed,
      filter_error,
      existing_acl == nullptr ? "null DACL is unsupported" : "copy ACL without sandbox SID");
  }
  auto* base_acl = reinterpret_cast<PACL>(filtered_acl_storage.data());

  PACL raw_acl = nullptr;
  if (!entries.empty()) {
    const auto acl_error =
      SetEntriesInAclW(static_cast<ULONG>(entries.size()), entries.data(), base_acl, &raw_acl);
    if (acl_error != ERROR_SUCCESS) {
      return make_acl_result(
        restricted_acl_grant_status::build_acl_failed, acl_error, "SetEntriesInAclW");
    }
  }

  struct acl_cleanup {
    PACL acl;
    ~acl_cleanup() {
      if (acl != nullptr) {
        LocalFree(acl);
      }
    }
  } cleanup { raw_acl };

  const auto security_flags =
    DACL_SECURITY_INFORMATION |
    ((kind == acl_update_kind::protect_read_only || kind == acl_update_kind::deny)
        ? PROTECTED_DACL_SECURITY_INFORMATION
        : 0);
  auto* updated_acl = entries.empty() ? base_acl : raw_acl;
  const auto security_error =
    SetSecurityInfo(handle, SE_FILE_OBJECT, security_flags, nullptr, nullptr, updated_acl, nullptr);
  if (security_error != ERROR_SUCCESS) {
    return make_acl_result(
      restricted_acl_grant_status::set_dacl_failed, security_error, path.string());
  }
  restricted_acl_grant_result result;
  if (directory) {
    result.directories_granted = 1;
  }
  else {
    result.files_granted = 1;
  }
  return result;
}

restricted_acl_grant_result remove_sid_from_path(
  const std::filesystem::path& path, PSID sid, bool directory) {
  restricted_windows_locked_path locked;
  const auto lock_result = lock_restricted_windows_path(path,
    READ_CONTROL | WRITE_DAC,
    directory ? restricted_windows_path_kind::directory : restricted_windows_path_kind::file,
    false,
    locked);
  if (!lock_result) {
    return acl_result_from_path(lock_result, restricted_acl_grant_status::revoke_failed);
  }
  const auto handle = locked.leaf_handle();

  PACL existing_acl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const auto query_error = GetSecurityInfo(handle,
    SE_FILE_OBJECT,
    DACL_SECURITY_INFORMATION,
    nullptr,
    nullptr,
    &existing_acl,
    nullptr,
    &descriptor);
  if (query_error != ERROR_SUCCESS) {
    return make_acl_result(restricted_acl_grant_status::revoke_failed, query_error, path.string());
  }
  struct descriptor_cleanup {
    PSECURITY_DESCRIPTOR descriptor;
    ~descriptor_cleanup() {
      if (descriptor != nullptr) {
        LocalFree(descriptor);
      }
    }
  } cleanup_descriptor { descriptor };
  if (existing_acl == nullptr) {
    return {};
  }

  std::vector<DWORD> filtered_acl_storage;
  DWORD filter_error = ERROR_SUCCESS;
  if (!copy_acl_without_sid(existing_acl, sid, filtered_acl_storage, filter_error)) {
    return make_acl_result(restricted_acl_grant_status::revoke_failed, filter_error, path.string());
  }
  auto* filtered_acl = reinterpret_cast<PACL>(filtered_acl_storage.data());
  const auto set_error = SetSecurityInfo(
    handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, filtered_acl, nullptr);
  if (set_error != ERROR_SUCCESS) {
    return make_acl_result(restricted_acl_grant_status::revoke_failed, set_error, path.string());
  }
  return {};
}

restricted_acl_grant_result remove_sid_from_tree(const std::filesystem::path& root, PSID sid) {
  restricted_windows_locked_path root_lock;
  const auto root_lock_result =
    lock_restricted_windows_path(root, 0, restricted_windows_path_kind::any, false, root_lock);
  if (!root_lock_result) {
    if (root_lock_result.status == restricted_windows_path_status::path_not_found) {
      return {};
    }
    return acl_result_from_path(root_lock_result, restricted_acl_grant_status::revoke_failed);
  }
  const auto root_directory = root_lock.is_directory();
  auto result = remove_sid_from_path(root, sid, root_directory);
  if (result.status != restricted_acl_grant_status::ok || !root_directory) {
    return result;
  }

  std::error_code error;
  std::filesystem::recursive_directory_iterator it(
    root, std::filesystem::directory_options::skip_permission_denied, error);
  if (error) {
    return make_acl_result(
      restricted_acl_grant_status::revoke_failed, error.value(), root.string());
  }
  for (std::filesystem::recursive_directory_iterator end; it != end; it.increment(error)) {
    if (error) {
      return make_acl_result(
        restricted_acl_grant_status::revoke_failed, error.value(), root.string());
    }
    std::error_code entry_error;
    const auto directory = it->is_directory(entry_error);
    if (entry_error) {
      return make_acl_result(
        restricted_acl_grant_status::revoke_failed, entry_error.value(), it->path().string());
    }
    auto removed = remove_sid_from_path(it->path(), sid, directory);
    if (removed.status != restricted_acl_grant_status::ok) {
      return removed;
    }
  }
  return {};
}

void merge_counts(restricted_acl_grant_result& target, const restricted_acl_grant_result& source) {
  target.directories_granted += source.directories_granted;
  target.files_granted += source.files_granted;
}

restricted_acl_grant_result update_tree_access(
  const restricted_acl_grant_request& request, acl_update_kind kind) {
  if (request.lease != nullptr) {
    request.lease->track_scope(request.path, request.sid);
  }
  restricted_windows_locked_path root_lock;
  const auto root_lock_result = lock_restricted_windows_path(
    request.path, 0, restricted_windows_path_kind::any, false, root_lock);
  if (!root_lock_result) {
    return acl_result_from_path(root_lock_result);
  }
  const auto root_is_directory = root_lock.is_directory();
  auto result = update_path_access(request.path,
    request.sid,
    root_is_directory ? request.directory_access : request.file_access,
    root_is_directory,
    kind,
    request.lease);
  if (result.status != restricted_acl_grant_status::ok) {
    return result;
  }
  if (!root_is_directory) {
    return result;
  }

  std::error_code error;
  std::filesystem::recursive_directory_iterator it(
    request.path, std::filesystem::directory_options::skip_permission_denied, error);
  if (error) {
    return make_acl_result(
      restricted_acl_grant_status::iterate_failed, error.value(), request.path.string());
  }

  for (std::filesystem::recursive_directory_iterator end; it != end; it.increment(error)) {
    if (error) {
      return make_acl_result(
        restricted_acl_grant_status::iterate_failed, error.value(), request.path.string());
    }

    std::error_code entry_error;
    const auto directory = it->is_directory(entry_error);
    if (entry_error) {
      return make_acl_result(
        restricted_acl_grant_status::iterate_failed, entry_error.value(), it->path().string());
    }
    if (!directory && !it->is_regular_file(entry_error)) {
      if (entry_error) {
        return make_acl_result(
          restricted_acl_grant_status::iterate_failed, entry_error.value(), it->path().string());
      }
      continue;
    }

    auto updated = update_path_access(it->path(),
      request.sid,
      directory ? request.directory_access : request.file_access,
      directory,
      kind,
      request.lease);
    if (updated.status != restricted_acl_grant_status::ok) {
      return updated;
    }
    merge_counts(result, updated);
  }
  return result;
}

} // namespace

restricted_acl_lease::~restricted_acl_lease() {
  for (int attempt = 0; attempt < 3 && (!snapshots_.empty() || !scope_roots_.empty()); ++attempt) {
    (void)restore();
  }
}

restricted_acl_lease::restricted_acl_lease(restricted_acl_lease&& other) noexcept
    : snapshots_(std::move(other.snapshots_)), scope_roots_(std::move(other.scope_roots_)),
      sid_(std::move(other.sid_)) {
  other.snapshots_.clear();
  other.scope_roots_.clear();
  other.sid_.clear();
}

restricted_acl_lease& restricted_acl_lease::operator=(restricted_acl_lease&& other) noexcept {
  if (this != &other) {
    if (restore().status != restricted_acl_grant_status::ok) {
      return *this;
    }
    snapshots_ = std::move(other.snapshots_);
    scope_roots_ = std::move(other.scope_roots_);
    sid_ = std::move(other.sid_);
    other.snapshots_.clear();
    other.scope_roots_.clear();
    other.sid_.clear();
  }
  return *this;
}

void restricted_acl_lease::track(
  const std::filesystem::path& path, PACL dacl, SECURITY_DESCRIPTOR_CONTROL control) {
  const auto normalized = path.lexically_normal();
  const auto existing = std::find_if(snapshots_.begin(), snapshots_.end(), [&](const auto& item) {
    return item.path == normalized;
  });
  if (existing != snapshots_.end()) {
    return;
  }
  snapshot item {
    .path = normalized,
    .null_dacl = dacl == nullptr,
    .protected_dacl = (control & SE_DACL_PROTECTED) != 0,
  };
  if (dacl != nullptr) {
    item.dacl.resize((dacl->AclSize + sizeof(DWORD) - 1) / sizeof(DWORD));
    std::memcpy(item.dacl.data(), dacl, dacl->AclSize);
  }
  snapshots_.push_back(std::move(item));
}

void restricted_acl_lease::track_scope(const std::filesystem::path& path, PSID sid) {
  if (sid == nullptr || !IsValidSid(sid)) {
    return;
  }
  if (sid_.empty()) {
    const auto sid_bytes = GetLengthSid(sid);
    sid_.resize((sid_bytes + sizeof(DWORD) - 1) / sizeof(DWORD));
    if (!CopySid(sid_bytes, sid_.data(), sid)) {
      sid_.clear();
      return;
    }
  }
  const auto normalized = path.lexically_normal();
  if (std::find(scope_roots_.begin(), scope_roots_.end(), normalized) == scope_roots_.end()) {
    scope_roots_.push_back(normalized);
  }
}

restricted_acl_grant_result restricted_acl_lease::restore() noexcept {
  restricted_acl_grant_result first_failure;
  if (!sid_.empty()) {
    for (std::size_t index = scope_roots_.size(); index > 0; --index) {
      auto removed = remove_sid_from_tree(scope_roots_[index - 1], sid_.data());
      if (removed.status != restricted_acl_grant_status::ok &&
          first_failure.status == restricted_acl_grant_status::ok) {
        first_failure = std::move(removed);
      }
      if (removed.status == restricted_acl_grant_status::ok) {
        scope_roots_.erase(scope_roots_.begin() + static_cast<std::ptrdiff_t>(index - 1));
      }
    }
  }
  for (std::size_t index = snapshots_.size(); index > 0; --index) {
    auto& snapshot = snapshots_[index - 1];
    restricted_windows_locked_path locked;
    const auto lock_result = lock_restricted_windows_path(
      snapshot.path, READ_CONTROL | WRITE_DAC, restricted_windows_path_kind::any, false, locked);
    if (!lock_result && lock_result.status == restricted_windows_path_status::path_not_found) {
      snapshots_.erase(snapshots_.begin() + static_cast<std::ptrdiff_t>(index - 1));
      continue;
    }
    if (!lock_result) {
      if (first_failure.status == restricted_acl_grant_status::ok) {
        first_failure =
          acl_result_from_path(lock_result, restricted_acl_grant_status::revoke_failed);
      }
      continue;
    }
    auto* original_acl =
      snapshot.null_dacl ? nullptr : reinterpret_cast<PACL>(snapshot.dacl.data());
    const auto security_flags =
      DACL_SECURITY_INFORMATION | (snapshot.protected_dacl ? PROTECTED_DACL_SECURITY_INFORMATION
                                                           : UNPROTECTED_DACL_SECURITY_INFORMATION);
    const auto restore_error = SetSecurityInfo(locked.leaf_handle(),
      SE_FILE_OBJECT,
      security_flags,
      nullptr,
      nullptr,
      original_acl,
      nullptr);
    if (restore_error != ERROR_SUCCESS && first_failure.status == restricted_acl_grant_status::ok) {
      first_failure = make_acl_result(
        restricted_acl_grant_status::revoke_failed, restore_error, snapshot.path.string());
    }
    if (restore_error == ERROR_SUCCESS) {
      snapshots_.erase(snapshots_.begin() + static_cast<std::ptrdiff_t>(index - 1));
    }
  }
  if (snapshots_.empty() && scope_roots_.empty()) {
    sid_.clear();
  }
  return first_failure;
}

const char* to_string(restricted_acl_grant_status status) noexcept {
  switch (status) {
    case restricted_acl_grant_status::ok:
      return "ok";
    case restricted_acl_grant_status::invalid_sid:
      return "invalid_sid";
    case restricted_acl_grant_status::empty_path:
      return "empty_path";
    case restricted_acl_grant_status::path_not_found:
      return "path_not_found";
    case restricted_acl_grant_status::current_user_sid_failed:
      return "current_user_sid_failed";
    case restricted_acl_grant_status::well_known_sid_failed:
      return "well_known_sid_failed";
    case restricted_acl_grant_status::build_acl_failed:
      return "build_acl_failed";
    case restricted_acl_grant_status::set_dacl_failed:
      return "set_dacl_failed";
    case restricted_acl_grant_status::iterate_failed:
      return "iterate_failed";
    case restricted_acl_grant_status::reparse_point_not_allowed:
      return "reparse_point_not_allowed";
    case restricted_acl_grant_status::hard_link_not_allowed:
      return "hard_link_not_allowed";
    case restricted_acl_grant_status::revoke_failed:
      return "revoke_failed";
  }
  return "unknown";
}

restricted_acl_grant_result grant_restricted_file_access(
  const std::filesystem::path& path, PSID sid, DWORD access_permissions) {
  return update_path_access(path, sid, access_permissions, false, acl_update_kind::allow);
}

restricted_acl_grant_result grant_restricted_directory_access(
  const std::filesystem::path& path, PSID sid, DWORD access_permissions) {
  return update_path_access(path, sid, access_permissions, true, acl_update_kind::allow);
}

restricted_acl_grant_result grant_restricted_tree_access(
  const restricted_acl_grant_request& request) {
  return update_tree_access(request, acl_update_kind::allow);
}

restricted_acl_grant_result protect_restricted_tree_read_only(
  const restricted_acl_grant_request& request) {
  return update_tree_access(request, acl_update_kind::protect_read_only);
}

restricted_acl_grant_result deny_restricted_tree_access(
  const restricted_acl_grant_request& request) {
  return update_tree_access(request, acl_update_kind::deny);
}

} // namespace wuwe::agent::execution::detail

#endif // _WIN32
