#include "restricted_process_request_workspace.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "restricted_process_path_win32.hpp"
#endif

namespace wuwe::agent::execution::detail {
namespace {

std::atomic<unsigned long long> workspace_counter {};

restricted_request_workspace_result make_workspace_result(
  restricted_request_workspace_status status, std::error_code error = {}, std::string detail = {}) {
  return {
    .status = status,
    .system_error = error,
    .detail = std::move(detail),
  };
}

std::string process_fragment() {
#ifdef _WIN32
  return std::to_string(GetCurrentProcessId());
#else
  return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

std::filesystem::path make_request_directory(
  const std::filesystem::path& root, const std::string& prefix) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto counter = workspace_counter.fetch_add(1, std::memory_order_relaxed);
  return root / (prefix + "-" + process_fragment() + "-" + std::to_string(now) + "-" +
                  std::to_string(counter));
}

bool is_safe_relative_script_filename(const std::filesystem::path& path) {
  if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
    return false;
  }
  for (const auto& part : path) {
    if (part == "..") {
      return false;
    }
  }
  return true;
}

} // namespace

restricted_request_workspace::~restricted_request_workspace() {
  reset();
}

restricted_request_workspace::restricted_request_workspace(
  restricted_request_workspace&& other) noexcept
    : root_(std::move(other.root_)), script_path_(std::move(other.script_path_)),
      cleanup_on_destroy_(std::exchange(other.cleanup_on_destroy_, false))
#ifdef _WIN32
      ,
      root_lock_(std::move(other.root_lock_)), script_lock_(std::move(other.script_lock_))
#endif
{
}

restricted_request_workspace& restricted_request_workspace::operator=(
  restricted_request_workspace&& other) noexcept {
  if (this != &other) {
    reset();
    root_ = std::move(other.root_);
    script_path_ = std::move(other.script_path_);
    cleanup_on_destroy_ = std::exchange(other.cleanup_on_destroy_, false);
#ifdef _WIN32
    root_lock_ = std::move(other.root_lock_);
    script_lock_ = std::move(other.script_lock_);
#endif
  }
  return *this;
}

void restricted_request_workspace::reset() noexcept {
  close_security_locks();
  if (cleanup_on_destroy_ && !root_.empty()) {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }
  root_.clear();
  script_path_.clear();
  cleanup_on_destroy_ = false;
}

void restricted_request_workspace::close_security_locks() noexcept {
#ifdef _WIN32
  script_lock_.reset();
  root_lock_.reset();
#endif
}

const char* to_string(restricted_request_workspace_status status) noexcept {
  switch (status) {
    case restricted_request_workspace_status::ok:
      return "ok";
    case restricted_request_workspace_status::empty_root:
      return "empty_root";
    case restricted_request_workspace_status::empty_script_filename:
      return "empty_script_filename";
    case restricted_request_workspace_status::invalid_script_filename:
      return "invalid_script_filename";
    case restricted_request_workspace_status::create_root_failed:
      return "create_root_failed";
    case restricted_request_workspace_status::create_request_directory_failed:
      return "create_request_directory_failed";
    case restricted_request_workspace_status::reparse_point_not_allowed:
      return "reparse_point_not_allowed";
    case restricted_request_workspace_status::write_script_failed:
      return "write_script_failed";
  }
  return "unknown";
}

restricted_request_workspace_result create_restricted_request_workspace(
  const restricted_request_workspace_request& request) {
  if (request.root.empty()) {
    return make_workspace_result(restricted_request_workspace_status::empty_root);
  }
  if (request.script_filename.empty()) {
    return make_workspace_result(restricted_request_workspace_status::empty_script_filename);
  }
  if (!is_safe_relative_script_filename(request.script_filename)) {
    return make_workspace_result(restricted_request_workspace_status::invalid_script_filename,
      {},
      request.script_filename.string());
  }

  std::error_code error;
#ifdef _WIN32
  restricted_windows_locked_path base_root_lock;
  const auto base_root_result = create_restricted_windows_directories(request.root, base_root_lock);
  if (!base_root_result) {
    const auto status =
      base_root_result.status == restricted_windows_path_status::reparse_point_not_allowed
        ? restricted_request_workspace_status::reparse_point_not_allowed
        : restricted_request_workspace_status::create_root_failed;
    return make_workspace_result(status,
      std::error_code(static_cast<int>(base_root_result.win32_error), std::system_category()),
      base_root_result.path.string());
  }
#else
  std::filesystem::create_directories(request.root, error);
  if (error) {
    return make_workspace_result(
      restricted_request_workspace_status::create_root_failed, error, request.root.string());
  }
#endif

  restricted_request_workspace workspace;
  workspace.root_ = make_request_directory(request.root,
    request.directory_prefix.empty() ? std::string("wuwe-restricted-request")
                                     : request.directory_prefix);
  workspace.script_path_ = workspace.root_ / request.script_filename;
  workspace.cleanup_on_destroy_ = request.cleanup_on_destroy;

#ifdef _WIN32
  if (!CreateDirectoryW(workspace.root_.wstring().c_str(), nullptr)) {
    return make_workspace_result(
      restricted_request_workspace_status::create_request_directory_failed,
      std::error_code(static_cast<int>(GetLastError()), std::system_category()),
      workspace.root_.string());
  }
  workspace.root_lock_ = std::make_unique<restricted_windows_locked_path>();
  const auto workspace_root_result = lock_restricted_windows_path(
    workspace.root_, 0, restricted_windows_path_kind::directory, false, *workspace.root_lock_);
  if (!workspace_root_result) {
    return make_workspace_result(
      restricted_request_workspace_status::create_request_directory_failed,
      std::error_code(static_cast<int>(workspace_root_result.win32_error), std::system_category()),
      workspace_root_result.path.string());
  }
  workspace.script_lock_ = std::make_unique<restricted_windows_locked_path>();
  const auto script_result = create_restricted_windows_file(workspace.script_path_,
    GENERIC_READ | GENERIC_WRITE,
    FILE_SHARE_READ,
    request.script_text,
    *workspace.script_lock_);
  if (!script_result) {
    const auto status =
      script_result.status == restricted_windows_path_status::reparse_point_not_allowed
        ? restricted_request_workspace_status::reparse_point_not_allowed
        : restricted_request_workspace_status::write_script_failed;
    return make_workspace_result(status,
      std::error_code(static_cast<int>(script_result.win32_error), std::system_category()),
      script_result.path.string());
  }
#else
  std::filesystem::create_directories(workspace.script_path_.parent_path(), error);
  if (error) {
    return make_workspace_result(
      restricted_request_workspace_status::create_request_directory_failed,
      error,
      workspace.root_.string());
  }
  {
    std::ofstream script(workspace.script_path_, std::ios::binary);
    if (!static_cast<bool>(script << request.script_text)) {
      return make_workspace_result(restricted_request_workspace_status::write_script_failed,
        {},
        workspace.script_path_.string());
    }
  }
#endif

  return {
    .status = restricted_request_workspace_status::ok,
    .workspace = std::move(workspace),
  };
}

} // namespace wuwe::agent::execution::detail
