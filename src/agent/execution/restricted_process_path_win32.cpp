#include "restricted_process_path_win32.hpp"

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <system_error>
#include <utility>

namespace wuwe::agent::execution::detail {
namespace {

restricted_windows_path_result make_path_result(restricted_windows_path_status status,
  const std::filesystem::path& path = {}, DWORD error = ERROR_SUCCESS, std::string detail = {}) {
  return {
    .status = status,
    .win32_error = error,
    .path = path,
    .detail = std::move(detail),
  };
}

std::optional<std::filesystem::path> absolute_normalized(
  const std::filesystem::path& path, restricted_windows_path_result& result) {
  if (path.empty()) {
    result = make_path_result(restricted_windows_path_status::empty_path);
    return std::nullopt;
  }
  std::error_code error;
  auto absolute = path.is_absolute() ? path : std::filesystem::absolute(path, error);
  if (error) {
    result = make_path_result(restricted_windows_path_status::absolute_path_failed,
      path,
      static_cast<DWORD>(error.value()),
      error.message());
    return std::nullopt;
  }
  return absolute.lexically_normal();
}

restricted_windows_path_result validate_handle(HANDLE handle, const std::filesystem::path& path,
  restricted_windows_path_kind expected_kind, bool reject_hard_links, bool& directory) {
  FILE_ATTRIBUTE_TAG_INFO tag {};
  if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
    return make_path_result(
      restricted_windows_path_status::open_failed, path, GetLastError(), "FileAttributeTagInfo");
  }
  if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return make_path_result(restricted_windows_path_status::reparse_point_not_allowed,
      path,
      ERROR_CANT_ACCESS_FILE,
      "reparse point component");
  }
  directory = (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  if ((expected_kind == restricted_windows_path_kind::directory && !directory) ||
      (expected_kind == restricted_windows_path_kind::file && directory)) {
    return make_path_result(restricted_windows_path_status::unexpected_file_type,
      path,
      directory ? ERROR_DIRECTORY : ERROR_FILE_NOT_FOUND,
      directory ? "expected file" : "expected directory");
  }
  if (!directory && reject_hard_links) {
    BY_HANDLE_FILE_INFORMATION information {};
    if (!GetFileInformationByHandle(handle, &information)) {
      return make_path_result(restricted_windows_path_status::open_failed,
        path,
        GetLastError(),
        "GetFileInformationByHandle");
    }
    if (information.nNumberOfLinks > 1) {
      return make_path_result(restricted_windows_path_status::hard_link_not_allowed,
        path,
        ERROR_CANT_ACCESS_FILE,
        "multiple hard links");
    }
  }
  return {};
}

restricted_windows_path_result open_component(const std::filesystem::path& path, DWORD access,
  DWORD share_mode, restricted_windows_path_kind expected_kind, bool reject_hard_links,
  HANDLE& handle, bool& directory) {
  const auto flags =
    FILE_FLAG_OPEN_REPARSE_POINT |
    (expected_kind == restricted_windows_path_kind::file ? 0 : FILE_FLAG_BACKUP_SEMANTICS);
  handle = CreateFileW(path.wstring().c_str(),
    access | FILE_READ_ATTRIBUTES,
    share_mode,
    nullptr,
    OPEN_EXISTING,
    flags,
    nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const auto error = GetLastError();
    return make_path_result(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                              ? restricted_windows_path_status::path_not_found
                              : restricted_windows_path_status::open_failed,
      path,
      error,
      "CreateFileW");
  }
  auto validated = validate_handle(handle, path, expected_kind, reject_hard_links, directory);
  if (!validated) {
    CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;
  }
  return validated;
}

} // namespace

struct restricted_windows_path_builder {
  static restricted_windows_path_result lock_directory_chain(const std::filesystem::path& requested,
    bool create_missing, restricted_windows_locked_path& locked) {
    locked.reset();
    restricted_windows_path_result result;
    const auto absolute = absolute_normalized(requested, result);
    if (!absolute) {
      return result;
    }

    auto current = absolute->root_path();
    if (current.empty()) {
      return make_path_result(
        restricted_windows_path_status::absolute_path_failed, *absolute, ERROR_INVALID_NAME);
    }

    HANDLE root_handle = INVALID_HANDLE_VALUE;
    bool root_directory = false;
    result = open_component(current,
      0,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      restricted_windows_path_kind::directory,
      false,
      root_handle,
      root_directory);
    if (!result) {
      return result;
    }
    locked.handles_.push_back(root_handle);

    for (const auto& component : absolute->relative_path()) {
      current /= component;
      HANDLE handle = INVALID_HANDLE_VALUE;
      bool directory = false;
      result = open_component(current,
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        restricted_windows_path_kind::directory,
        false,
        handle,
        directory);
      if (!result && create_missing &&
          result.status == restricted_windows_path_status::path_not_found) {
        if (!CreateDirectoryW(current.wstring().c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
          const auto error = GetLastError();
          locked.reset();
          return make_path_result(restricted_windows_path_status::create_directory_failed,
            current,
            error,
            "CreateDirectoryW");
        }
        result = open_component(current,
          0,
          FILE_SHARE_READ | FILE_SHARE_WRITE,
          restricted_windows_path_kind::directory,
          false,
          handle,
          directory);
      }
      if (!result) {
        locked.reset();
        return result;
      }
      locked.handles_.push_back(handle);
    }

    locked.path_ = *absolute;
    locked.directory_ = true;
    return {};
  }
};

namespace {

restricted_windows_path_result write_all(
  HANDLE handle, std::string_view contents, const std::filesystem::path& path) {
  const char* cursor = contents.data();
  std::size_t remaining = contents.size();
  while (remaining > 0) {
    const auto chunk = static_cast<DWORD>((std::min<std::size_t>)(remaining, 1U << 20));
    DWORD written = 0;
    if (!WriteFile(handle, cursor, chunk, &written, nullptr) || written == 0) {
      return make_path_result(
        restricted_windows_path_status::write_failed, path, GetLastError(), "WriteFile");
    }
    cursor += written;
    remaining -= written;
  }
  return {};
}

} // namespace

restricted_windows_locked_path::~restricted_windows_locked_path() {
  reset();
}

restricted_windows_locked_path::restricted_windows_locked_path(
  restricted_windows_locked_path&& other) noexcept
    : path_(std::move(other.path_)), handles_(std::move(other.handles_)),
      directory_(std::exchange(other.directory_, false)) {
  other.handles_.clear();
}

restricted_windows_locked_path& restricted_windows_locked_path::operator=(
  restricted_windows_locked_path&& other) noexcept {
  if (this != &other) {
    reset();
    path_ = std::move(other.path_);
    handles_ = std::move(other.handles_);
    directory_ = std::exchange(other.directory_, false);
    other.handles_.clear();
  }
  return *this;
}

HANDLE restricted_windows_locked_path::leaf_handle() const noexcept {
  return handles_.empty() ? INVALID_HANDLE_VALUE : handles_.back();
}

const std::filesystem::path& restricted_windows_locked_path::path() const noexcept {
  return path_;
}

bool restricted_windows_locked_path::is_directory() const noexcept {
  return directory_;
}

bool restricted_windows_locked_path::valid() const noexcept {
  return !handles_.empty() && leaf_handle() != INVALID_HANDLE_VALUE;
}

void restricted_windows_locked_path::reset() noexcept {
  for (auto it = handles_.rbegin(); it != handles_.rend(); ++it) {
    if (*it != nullptr && *it != INVALID_HANDLE_VALUE) {
      CloseHandle(*it);
    }
  }
  handles_.clear();
  path_.clear();
  directory_ = false;
}

const char* to_string(restricted_windows_path_status status) noexcept {
  switch (status) {
    case restricted_windows_path_status::ok:
      return "ok";
    case restricted_windows_path_status::empty_path:
      return "empty_path";
    case restricted_windows_path_status::absolute_path_failed:
      return "absolute_path_failed";
    case restricted_windows_path_status::path_not_found:
      return "path_not_found";
    case restricted_windows_path_status::open_failed:
      return "open_failed";
    case restricted_windows_path_status::reparse_point_not_allowed:
      return "reparse_point_not_allowed";
    case restricted_windows_path_status::unexpected_file_type:
      return "unexpected_file_type";
    case restricted_windows_path_status::hard_link_not_allowed:
      return "hard_link_not_allowed";
    case restricted_windows_path_status::create_directory_failed:
      return "create_directory_failed";
    case restricted_windows_path_status::create_file_failed:
      return "create_file_failed";
    case restricted_windows_path_status::read_failed:
      return "read_failed";
    case restricted_windows_path_status::write_failed:
      return "write_failed";
  }
  return "unknown";
}

restricted_windows_path_result lock_restricted_windows_path(const std::filesystem::path& path,
  DWORD leaf_access, restricted_windows_path_kind expected_kind, bool reject_hard_links,
  restricted_windows_locked_path& locked, DWORD leaf_share_mode) {
  locked.reset();
  restricted_windows_path_result result;
  const auto absolute = absolute_normalized(path, result);
  if (!absolute) {
    return result;
  }
  if (*absolute == absolute->root_path()) {
    return restricted_windows_path_builder::lock_directory_chain(*absolute, false, locked);
  }

  auto parent = absolute->parent_path();
  result = restricted_windows_path_builder::lock_directory_chain(parent, false, locked);
  if (!result) {
    return result;
  }

  HANDLE leaf = INVALID_HANDLE_VALUE;
  bool directory = false;
  result = open_component(
    *absolute, leaf_access, leaf_share_mode, expected_kind, reject_hard_links, leaf, directory);
  if (!result) {
    locked.reset();
    return result;
  }
  locked.handles_.push_back(leaf);
  locked.path_ = *absolute;
  locked.directory_ = directory;
  return {};
}

restricted_windows_path_result create_restricted_windows_directories(
  const std::filesystem::path& path, restricted_windows_locked_path& locked) {
  return restricted_windows_path_builder::lock_directory_chain(path, true, locked);
}

restricted_windows_path_result create_restricted_windows_file(const std::filesystem::path& path,
  DWORD access, DWORD share_mode, std::string_view contents,
  restricted_windows_locked_path& locked) {
  locked.reset();
  restricted_windows_path_result result;
  const auto absolute = absolute_normalized(path, result);
  if (!absolute || absolute->parent_path().empty()) {
    return absolute ? make_path_result(restricted_windows_path_status::absolute_path_failed,
                        *absolute,
                        ERROR_INVALID_NAME)
                    : result;
  }

  result =
    restricted_windows_path_builder::lock_directory_chain(absolute->parent_path(), true, locked);
  if (!result) {
    return result;
  }
  HANDLE file = CreateFileW(absolute->wstring().c_str(),
    access | FILE_READ_ATTRIBUTES,
    share_mode,
    nullptr,
    CREATE_NEW,
    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
    nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const auto error = GetLastError();
    locked.reset();
    return make_path_result(
      restricted_windows_path_status::create_file_failed, *absolute, error, "CreateFileW");
  }
  bool directory = false;
  result = validate_handle(file, *absolute, restricted_windows_path_kind::file, true, directory);
  if (!result) {
    CloseHandle(file);
    locked.reset();
    return result;
  }
  result = write_all(file, contents, *absolute);
  if (!result) {
    CloseHandle(file);
    locked.reset();
    return result;
  }
  locked.handles_.push_back(file);
  locked.path_ = *absolute;
  locked.directory_ = false;
  return {};
}

restricted_windows_path_result copy_restricted_windows_file(
  const std::filesystem::path& source, const std::filesystem::path& destination) {
  restricted_windows_locked_path source_lock;
  auto result = lock_restricted_windows_path(source,
    GENERIC_READ,
    restricted_windows_path_kind::file,
    true,
    source_lock,
    FILE_SHARE_READ | FILE_SHARE_WRITE);
  if (!result) {
    return result;
  }

  restricted_windows_locked_path destination_lock;
  result = create_restricted_windows_file(
    destination, GENERIC_WRITE, FILE_SHARE_READ, {}, destination_lock);
  if (!result) {
    return result;
  }

  std::array<char, 64 * 1024> buffer {};
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(source_lock.leaf_handle(),
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &read,
          nullptr)) {
      return make_path_result(
        restricted_windows_path_status::read_failed, source, GetLastError(), "ReadFile");
    }
    if (read == 0) {
      break;
    }
    auto write_result = write_all(destination_lock.leaf_handle(),
      std::string_view(buffer.data(), static_cast<std::size_t>(read)),
      destination);
    if (!write_result) {
      return write_result;
    }
  }
  return {};
}

} // namespace wuwe::agent::execution::detail

#endif // _WIN32
