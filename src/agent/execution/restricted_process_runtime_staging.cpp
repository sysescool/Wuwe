#include "restricted_process_runtime_staging.hpp"

#include <algorithm>
#include <cwctype>
#include <fstream>
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

restricted_python_runtime_staging_result make_result(
  const restricted_python_runtime_staging_request& request,
  restricted_python_runtime_staging_status status, std::error_code error = {},
  std::string detail = {}) {
  return {
    .status = status,
    .source_home = request.source_python.parent_path(),
    .destination_home = request.destination_home,
    .python_executable = request.destination_home / request.source_python.filename(),
    .system_error = error,
    .detail = std::move(detail),
  };
}

#ifdef _WIN32
std::wstring lowercase(std::wstring text) {
  std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return text;
}

bool is_windows_runtime_dll(const std::filesystem::path& path) {
  const auto extension = lowercase(path.extension().wstring());
  if (extension != L".dll") {
    return false;
  }

  const auto filename = lowercase(path.filename().wstring());
  return filename.rfind(L"python", 0) == 0 || filename.rfind(L"vcruntime", 0) == 0;
}

bool is_python_runtime_dll(const std::filesystem::path& path) {
  const auto extension = lowercase(path.extension().wstring());
  const auto filename = lowercase(path.filename().wstring());
  return extension == L".dll" && filename.rfind(L"python", 0) == 0;
}

std::filesystem::path resolve_source_python(const std::filesystem::path& source_python) {
  if (lowercase(source_python.filename().wstring()) != L"python3.exe") {
    return source_python;
  }

  // Some Windows distributions expose python3.exe as a redirector that must
  // remain beside python.exe. Stage the real interpreter when it is available.
  const auto python_executable = source_python.parent_path() / "python.exe";
  std::error_code error;
  if (std::filesystem::is_regular_file(python_executable, error)) {
    return python_executable;
  }
  return source_python;
}

bool write_python_path_configuration(
  const std::filesystem::path& path, restricted_python_runtime_staging_result& result) {
  // Keep the staged interpreter independent of machine-wide Python registry
  // entries, including unregistered installations used by CI tool caches.
  restricted_windows_locked_path locked;
  const auto write_result =
    create_restricted_windows_file(path, GENERIC_WRITE, FILE_SHARE_READ, "Lib\r\n", locked);
  if (!write_result) {
    result.status = restricted_python_runtime_staging_status::write_configuration_failed;
    result.system_error =
      std::error_code(static_cast<int>(write_result.win32_error), std::system_category());
    result.detail = path.string();
    return false;
  }
  return true;
}

bool copy_required_file(const std::filesystem::path& source,
  const std::filesystem::path& destination, restricted_python_runtime_staging_result& result) {
  const auto copied = copy_restricted_windows_file(source, destination);
  if (!copied) {
    if (copied.status == restricted_windows_path_status::reparse_point_not_allowed) {
      const auto source_text = source.lexically_normal().wstring();
      const auto failed_text = copied.path.lexically_normal().wstring();
      result.status = source_text.starts_with(failed_text)
                        ? restricted_python_runtime_staging_status::source_reparse_point
                        : restricted_python_runtime_staging_status::destination_reparse_point;
    }
    else if (copied.status == restricted_windows_path_status::create_directory_failed ||
             copied.status == restricted_windows_path_status::create_file_failed) {
      result.status = restricted_python_runtime_staging_status::create_destination_failed;
    }
    else {
      result.status = restricted_python_runtime_staging_status::copy_failed;
    }
    result.system_error =
      std::error_code(static_cast<int>(copied.win32_error), std::system_category());
    result.detail = copied.path.empty() ? source.string() : copied.path.string();
    return false;
  }
  result.copied_files.push_back(destination);
  return true;
}

bool copy_directory_tree(const std::filesystem::path& source,
  const std::filesystem::path& destination, restricted_python_runtime_staging_result& result) {
  restricted_windows_locked_path source_lock;
  const auto source_lock_result = lock_restricted_windows_path(
    source, 0, restricted_windows_path_kind::directory, false, source_lock);
  if (!source_lock_result) {
    if (source_lock_result.status == restricted_windows_path_status::reparse_point_not_allowed) {
      result.status = restricted_python_runtime_staging_status::source_reparse_point;
    }
    else {
      result.status = restricted_python_runtime_staging_status::source_lib_missing;
    }
    result.system_error =
      std::error_code(static_cast<int>(source_lock_result.win32_error), std::system_category());
    result.detail = source.string();
    return false;
  }
  restricted_windows_locked_path destination_lock;
  const auto destination_result =
    create_restricted_windows_directories(destination, destination_lock);
  if (!destination_result) {
    result.status = restricted_python_runtime_staging_status::create_destination_failed;
    result.system_error =
      std::error_code(static_cast<int>(destination_result.win32_error), std::system_category());
    result.detail = destination.string();
    return false;
  }

  std::error_code error;
  std::filesystem::recursive_directory_iterator it(source, error);
  if (error) {
    result.status = restricted_python_runtime_staging_status::copy_failed;
    result.system_error = error;
    result.detail = source.string();
    return false;
  }

  for (std::filesystem::recursive_directory_iterator end; it != end; it.increment(error)) {
    if (error) {
      result.status = restricted_python_runtime_staging_status::copy_failed;
      result.system_error = error;
      result.detail = source.string();
      return false;
    }

    const auto relative = it->path().lexically_relative(source);
    if (relative.empty()) {
      result.status = restricted_python_runtime_staging_status::copy_failed;
      result.detail = it->path().string();
      return false;
    }

    const auto target = destination / relative;
    if (it->is_directory(error)) {
      restricted_windows_locked_path source_directory_lock;
      const auto source_directory = lock_restricted_windows_path(
        it->path(), 0, restricted_windows_path_kind::directory, false, source_directory_lock);
      if (!source_directory) {
        result.status =
          source_directory.status == restricted_windows_path_status::reparse_point_not_allowed
            ? restricted_python_runtime_staging_status::source_reparse_point
            : restricted_python_runtime_staging_status::copy_failed;
        result.system_error =
          std::error_code(static_cast<int>(source_directory.win32_error), std::system_category());
        result.detail = it->path().string();
        return false;
      }
      restricted_windows_locked_path target_lock;
      const auto target_result = create_restricted_windows_directories(target, target_lock);
      if (!target_result) {
        result.status = restricted_python_runtime_staging_status::create_destination_failed;
        result.system_error =
          std::error_code(static_cast<int>(target_result.win32_error), std::system_category());
        result.detail = target.string();
        return false;
      }
      continue;
    }
    if (error) {
      result.status = restricted_python_runtime_staging_status::copy_failed;
      result.system_error = error;
      result.detail = it->path().string();
      return false;
    }

    if (it->is_regular_file(error)) {
      if (!copy_required_file(it->path(), target, result)) {
        return false;
      }
    }
    if (error) {
      result.status = restricted_python_runtime_staging_status::copy_failed;
      result.system_error = error;
      result.detail = it->path().string();
      return false;
    }
  }
  return true;
}
#endif

} // namespace

const char* to_string(restricted_python_runtime_staging_status status) noexcept {
  switch (status) {
    case restricted_python_runtime_staging_status::ok:
      return "ok";
    case restricted_python_runtime_staging_status::unsupported_platform:
      return "unsupported_platform";
    case restricted_python_runtime_staging_status::empty_source_python:
      return "empty_source_python";
    case restricted_python_runtime_staging_status::empty_destination_home:
      return "empty_destination_home";
    case restricted_python_runtime_staging_status::source_python_not_found:
      return "source_python_not_found";
    case restricted_python_runtime_staging_status::source_python_not_regular_file:
      return "source_python_not_regular_file";
    case restricted_python_runtime_staging_status::source_home_missing:
      return "source_home_missing";
    case restricted_python_runtime_staging_status::source_lib_missing:
      return "source_lib_missing";
    case restricted_python_runtime_staging_status::source_reparse_point:
      return "source_reparse_point";
    case restricted_python_runtime_staging_status::destination_reparse_point:
      return "destination_reparse_point";
    case restricted_python_runtime_staging_status::create_destination_failed:
      return "create_destination_failed";
    case restricted_python_runtime_staging_status::copy_failed:
      return "copy_failed";
    case restricted_python_runtime_staging_status::write_configuration_failed:
      return "write_configuration_failed";
  }
  return "unknown";
}

restricted_python_runtime_staging_result stage_minimal_python_runtime_for_restricted_process(
  const restricted_python_runtime_staging_request& request) {
  if (request.source_python.empty()) {
    return make_result(request, restricted_python_runtime_staging_status::empty_source_python);
  }
  if (request.destination_home.empty()) {
    return make_result(request, restricted_python_runtime_staging_status::empty_destination_home);
  }

#ifndef _WIN32
  return make_result(request, restricted_python_runtime_staging_status::unsupported_platform);
#else
  std::error_code error;
  const auto source_python = resolve_source_python(request.source_python);
  restricted_windows_locked_path source_python_lock;
  const auto source_python_result = lock_restricted_windows_path(
    source_python, GENERIC_READ, restricted_windows_path_kind::file, true, source_python_lock);
  if (!source_python_result) {
    auto status = restricted_python_runtime_staging_status::source_python_not_regular_file;
    if (source_python_result.status == restricted_windows_path_status::path_not_found) {
      status = restricted_python_runtime_staging_status::source_python_not_found;
    }
    else if (source_python_result.status ==
             restricted_windows_path_status::reparse_point_not_allowed) {
      status = restricted_python_runtime_staging_status::source_reparse_point;
    }
    return make_result(request,
      status,
      std::error_code(static_cast<int>(source_python_result.win32_error), std::system_category()),
      source_python_result.path.string());
  }
  const auto source_home = source_python.parent_path();
  restricted_windows_locked_path source_home_lock;
  const auto source_home_result = lock_restricted_windows_path(
    source_home, 0, restricted_windows_path_kind::directory, false, source_home_lock);
  if (source_home.empty() || !source_home_result) {
    return make_result(request,
      restricted_python_runtime_staging_status::source_home_missing,
      std::error_code(static_cast<int>(source_home_result.win32_error), std::system_category()),
      source_home.string());
  }

  auto result = make_result(request, restricted_python_runtime_staging_status::ok);
  result.source_home = source_home;
  result.python_executable = request.destination_home / source_python.filename();

  if (request.replace_existing) {
    restricted_windows_locked_path existing_destination;
    const auto existing_result = lock_restricted_windows_path(request.destination_home,
      0,
      restricted_windows_path_kind::directory,
      false,
      existing_destination);
    if (!existing_result &&
        existing_result.status != restricted_windows_path_status::path_not_found) {
      result.status =
        existing_result.status == restricted_windows_path_status::reparse_point_not_allowed
          ? restricted_python_runtime_staging_status::destination_reparse_point
          : restricted_python_runtime_staging_status::create_destination_failed;
      result.system_error =
        std::error_code(static_cast<int>(existing_result.win32_error), std::system_category());
      result.detail = existing_result.path.string();
      return result;
    }
    existing_destination.reset();
    std::filesystem::remove_all(request.destination_home, error);
    if (error) {
      result.status = restricted_python_runtime_staging_status::create_destination_failed;
      result.system_error = error;
      result.detail = request.destination_home.string();
      return result;
    }
  }

  restricted_windows_locked_path destination_home_lock;
  const auto destination_home_result =
    create_restricted_windows_directories(request.destination_home, destination_home_lock);
  if (!destination_home_result) {
    result.status = restricted_python_runtime_staging_status::create_destination_failed;
    result.system_error = std::error_code(
      static_cast<int>(destination_home_result.win32_error), std::system_category());
    result.detail = destination_home_result.path.string();
    return result;
  }

  if (!copy_required_file(source_python, result.python_executable, result)) {
    return result;
  }
  if (!write_python_path_configuration(
        request.destination_home / (source_python.stem().wstring() + L"._pth"), result)) {
    return result;
  }

  std::filesystem::directory_iterator it(source_home, error);
  if (error) {
    result.status = restricted_python_runtime_staging_status::copy_failed;
    result.system_error = error;
    result.detail = source_home.string();
    return result;
  }

  for (std::filesystem::directory_iterator end; it != end; it.increment(error)) {
    if (error) {
      result.status = restricted_python_runtime_staging_status::copy_failed;
      result.system_error = error;
      result.detail = source_home.string();
      return result;
    }

    if (it->is_regular_file(error) && is_windows_runtime_dll(it->path())) {
      if (!copy_required_file(
            it->path(), request.destination_home / it->path().filename(), result)) {
        return result;
      }
      if (is_python_runtime_dll(it->path()) &&
          !write_python_path_configuration(
            request.destination_home / (it->path().stem().wstring() + L"._pth"), result)) {
        return result;
      }
    }
    if (error) {
      result.status = restricted_python_runtime_staging_status::copy_failed;
      result.system_error = error;
      result.detail = it->path().string();
      return result;
    }
  }

  const auto source_lib = source_home / "Lib";
  if (!copy_directory_tree(
        source_lib / "encodings", request.destination_home / "Lib" / "encodings", result)) {
    return result;
  }

  const auto codecs = source_lib / "codecs.py";
  if (std::filesystem::exists(codecs, error)) {
    if (!copy_required_file(codecs, request.destination_home / "Lib" / "codecs.py", result)) {
      return result;
    }
  }
  if (error) {
    result.status = restricted_python_runtime_staging_status::copy_failed;
    result.system_error = error;
    result.detail = codecs.string();
    return result;
  }

  const auto os = source_lib / "os.py";
  if (std::filesystem::exists(os, error)) {
    if (!copy_required_file(os, request.destination_home / "Lib" / "os.py", result)) {
      return result;
    }
  }
  if (error) {
    result.status = restricted_python_runtime_staging_status::copy_failed;
    result.system_error = error;
    result.detail = os.string();
    return result;
  }

  return result;
#endif
}

} // namespace wuwe::agent::execution::detail
