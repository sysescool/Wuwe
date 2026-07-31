#include <wuwe/agent/filesystem/local_filesystem_backend.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <regex>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace wuwe::agent::filesystem {
namespace {

filesystem_result failure(
  filesystem_status status, const std::filesystem::path& path, std::string message) {
  return {
    .status = status,
    .error_message = std::move(message),
    .path = path,
  };
}

filesystem_result failure_with_progress(filesystem_result result, filesystem_status status,
  const std::filesystem::path& path, std::string message) {
  result.status = status;
  result.error_message = std::move(message);
  result.path = path;
  const auto destination_created = result.metadata.find("destination_created");
  const auto parents_created = result.metadata.find("parent_directories_created");
  const auto partial =
    result.affected_items > 0 || result.bytes_processed > 0 ||
    (destination_created != result.metadata.end() && destination_created->second == "true") ||
    (parents_created != result.metadata.end() && parents_created->second == "true");
  result.metadata["partial"] = partial ? "true" : "false";
  return result;
}

bool path_component_equal(const std::filesystem::path& left, const std::filesystem::path& right) {
#ifdef _WIN32
  auto left_text = left.wstring();
  auto right_text = right.wstring();
  std::transform(left_text.begin(), left_text.end(), left_text.begin(), ::towlower);
  std::transform(right_text.begin(), right_text.end(), right_text.begin(), ::towlower);
  return left_text == right_text;
#else
  return left == right;
#endif
}

bool path_within(const std::filesystem::path& candidate, const std::filesystem::path& root) {
  auto candidate_it = candidate.begin();
  for (auto root_it = root.begin(); root_it != root.end(); ++root_it, ++candidate_it) {
    if (candidate_it == candidate.end() || !path_component_equal(*candidate_it, *root_it)) {
      return false;
    }
  }
  return true;
}

bool existing_path_contains_symlink(const std::filesystem::path& boundary,
  const std::filesystem::path& target, std::filesystem::path& offending, std::error_code& error) {
  const auto normalized_boundary = boundary.lexically_normal();
  const auto normalized_target = target.lexically_normal();
  if (!path_within(normalized_target, normalized_boundary)) {
    error = std::make_error_code(std::errc::invalid_argument);
    return false;
  }
  auto current = normalized_boundary;
  bool current_exists = false;
  const auto inspect = [&]() {
    const auto status = std::filesystem::symlink_status(current, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
      current_exists = false;
      return false;
    }
    if (error)
      return false;
    current_exists = std::filesystem::exists(status);
    if (std::filesystem::is_symlink(status)) {
      offending = current;
      return true;
    }
    return false;
  };
  if (inspect())
    return true;
  if (error || !current_exists)
    return false;
  const auto relative = normalized_target.lexically_relative(normalized_boundary);
  for (const auto& component : relative) {
    current /= component;
    if (inspect())
      return true;
    if (error || !current_exists)
      return false;
  }
  return false;
}

std::filesystem::path normalized_for_comparison(
  const std::filesystem::path& path, std::error_code& error) {
  auto normalized = std::filesystem::weakly_canonical(path, error);
  if (!error)
    return normalized;
  error.clear();
  return std::filesystem::absolute(path, error).lexically_normal();
}

filesystem_status status_from_error(const std::error_code& error) {
  if (error == std::errc::no_such_file_or_directory) {
    return filesystem_status::not_found;
  }
  if (error == std::errc::file_exists) {
    return filesystem_status::already_exists;
  }
  if (error == std::errc::permission_denied) {
    return filesystem_status::permission_denied;
  }
  return filesystem_status::io_error;
}

bool valid_utf8(std::string_view value) {
  std::size_t index = 0;
  while (index < value.size()) {
    const auto lead = static_cast<unsigned char>(value[index]);
    if (lead == 0) {
      return false;
    }
    if (lead < 0x80) {
      ++index;
      continue;
    }
    std::size_t count = 0;
    std::uint32_t codepoint = 0;
    if ((lead & 0xE0U) == 0xC0U) {
      count = 2;
      codepoint = lead & 0x1FU;
      if (codepoint == 0)
        return false;
    }
    else if ((lead & 0xF0U) == 0xE0U) {
      count = 3;
      codepoint = lead & 0x0FU;
    }
    else if ((lead & 0xF8U) == 0xF0U) {
      count = 4;
      codepoint = lead & 0x07U;
    }
    else {
      return false;
    }
    if (index + count > value.size())
      return false;
    for (std::size_t offset = 1; offset < count; ++offset) {
      const auto byte = static_cast<unsigned char>(value[index + offset]);
      if ((byte & 0xC0U) != 0x80U)
        return false;
      codepoint = (codepoint << 6U) | (byte & 0x3FU);
    }
    if ((count == 2 && codepoint < 0x80U) || (count == 3 && codepoint < 0x800U) ||
        (count == 4 && codepoint < 0x10000U) || codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
      return false;
    }
    index += count;
  }
  return true;
}

constexpr std::array<std::uint32_t, 64> sha256_k {
  0x428a2f98U,
  0x71374491U,
  0xb5c0fbcfU,
  0xe9b5dba5U,
  0x3956c25bU,
  0x59f111f1U,
  0x923f82a4U,
  0xab1c5ed5U,
  0xd807aa98U,
  0x12835b01U,
  0x243185beU,
  0x550c7dc3U,
  0x72be5d74U,
  0x80deb1feU,
  0x9bdc06a7U,
  0xc19bf174U,
  0xe49b69c1U,
  0xefbe4786U,
  0x0fc19dc6U,
  0x240ca1ccU,
  0x2de92c6fU,
  0x4a7484aaU,
  0x5cb0a9dcU,
  0x76f988daU,
  0x983e5152U,
  0xa831c66dU,
  0xb00327c8U,
  0xbf597fc7U,
  0xc6e00bf3U,
  0xd5a79147U,
  0x06ca6351U,
  0x14292967U,
  0x27b70a85U,
  0x2e1b2138U,
  0x4d2c6dfcU,
  0x53380d13U,
  0x650a7354U,
  0x766a0abbU,
  0x81c2c92eU,
  0x92722c85U,
  0xa2bfe8a1U,
  0xa81a664bU,
  0xc24b8b70U,
  0xc76c51a3U,
  0xd192e819U,
  0xd6990624U,
  0xf40e3585U,
  0x106aa070U,
  0x19a4c116U,
  0x1e376c08U,
  0x2748774cU,
  0x34b0bcb5U,
  0x391c0cb3U,
  0x4ed8aa4aU,
  0x5b9cca4fU,
  0x682e6ff3U,
  0x748f82eeU,
  0x78a5636fU,
  0x84c87814U,
  0x8cc70208U,
  0x90befffaU,
  0xa4506cebU,
  0xbef9a3f7U,
  0xc67178f2U,
};

std::uint32_t rotate_right(std::uint32_t value, unsigned count) {
  return (value >> count) | (value << (32U - count));
}

std::string revision_for(std::string_view input) {
  std::vector<std::uint8_t> bytes(input.begin(), input.end());
  const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8U;
  bytes.push_back(0x80U);
  while ((bytes.size() % 64U) != 56U)
    bytes.push_back(0U);
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes.push_back(static_cast<std::uint8_t>(bit_length >> shift));
  }

  std::array<std::uint32_t, 8> state {
    0x6a09e667U,
    0xbb67ae85U,
    0x3c6ef372U,
    0xa54ff53aU,
    0x510e527fU,
    0x9b05688cU,
    0x1f83d9abU,
    0x5be0cd19U,
  };
  for (std::size_t block = 0; block < bytes.size(); block += 64U) {
    std::array<std::uint32_t, 64> words {};
    for (std::size_t i = 0; i < 16; ++i) {
      const auto offset = block + i * 4U;
      words[i] = (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
                 (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
                 (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
                 static_cast<std::uint32_t>(bytes[offset + 3]);
    }
    for (std::size_t i = 16; i < words.size(); ++i) {
      const auto s0 =
        rotate_right(words[i - 15], 7) ^ rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3U);
      const auto s1 =
        rotate_right(words[i - 2], 17) ^ rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10U);
      words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    auto a = state[0];
    auto b = state[1];
    auto c = state[2];
    auto d = state[3];
    auto e = state[4];
    auto f = state[5];
    auto g = state[6];
    auto h = state[7];
    for (std::size_t i = 0; i < words.size(); ++i) {
      const auto s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
      const auto choice = (e & f) ^ ((~e) & g);
      const auto temp1 = h + s1 + choice + sha256_k[i] + words[i];
      const auto s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = s0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }
  std::ostringstream output;
  output << "sha256:" << std::hex << std::setfill('0');
  for (const auto value : state)
    output << std::setw(8) << value;
  return output.str();
}

filesystem_result read_regular_file(const std::filesystem::path& path, std::size_t max_bytes,
  std::stop_token stop_token, bool require_utf8 = true) {
  if (stop_token.stop_requested()) {
    return failure(filesystem_status::cancelled, path, "operation cancelled");
  }
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error)
    return failure(status_from_error(error), path, error.message());
  if (!std::filesystem::exists(status)) {
    return failure(filesystem_status::not_found, path, "file does not exist");
  }
  if (!std::filesystem::is_regular_file(status)) {
    return failure(filesystem_status::type_mismatch, path, "path is not a regular file");
  }
  const auto size = std::filesystem::file_size(path, error);
  if (error)
    return failure(status_from_error(error), path, error.message());
  if (max_bytes > 0 && size > max_bytes) {
    return failure(filesystem_status::limit_exceeded, path, "file exceeds the read limit");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return failure(filesystem_status::io_error, path, "failed to open file");
  std::string content;
  content.resize(static_cast<std::size_t>(size));
  constexpr std::size_t chunk_size = 64 * 1024;
  std::size_t offset = 0;
  while (offset < content.size()) {
    if (stop_token.stop_requested()) {
      return failure(filesystem_status::cancelled, path, "operation cancelled");
    }
    const auto count = (std::min)(chunk_size, content.size() - offset);
    input.read(content.data() + offset, static_cast<std::streamsize>(count));
    if (input.gcount() != static_cast<std::streamsize>(count)) {
      return failure(filesystem_status::io_error, path, "file changed while it was being read");
    }
    offset += count;
  }
  if (require_utf8 && !valid_utf8(content)) {
    return failure(filesystem_status::type_mismatch, path, "file is not valid UTF-8 text");
  }
  const auto revision = revision_for(content);
  return {
    .status = filesystem_status::ok,
    .path = path,
    .content = std::move(content),
    .revision = revision,
    .bytes_processed = static_cast<std::size_t>(size),
    .affected_items = 1,
  };
}

std::filesystem::path temp_path_for(const std::filesystem::path& path) {
  static std::atomic_uint64_t next_id { 1 };
  auto result = path;
  result += ".wuwe-tmp-";
#ifdef _WIN32
  result += std::to_string(GetCurrentProcessId());
#else
  result += std::to_string(static_cast<unsigned long long>(::getpid()));
#endif
  result += "-" + std::to_string(next_id.fetch_add(1));
  return result;
}

filesystem_result create_temporary_file(const std::filesystem::path& destination,
  std::string_view content, std::stop_token stop_token, std::filesystem::path& temporary) {
  constexpr std::size_t max_attempts = 128;
  for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
    if (stop_token.stop_requested()) {
      return failure(filesystem_status::cancelled, destination, "operation cancelled");
    }
    temporary = temp_path_for(destination);
#ifdef _WIN32
    const auto handle = CreateFileW(
      temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      const auto native_error = GetLastError();
      if (native_error == ERROR_FILE_EXISTS || native_error == ERROR_ALREADY_EXISTS) {
        continue;
      }
      const std::error_code error(static_cast<int>(native_error), std::system_category());
      return failure(status_from_error(error),
        destination,
        "failed to create temporary file: " + error.message());
    }
    bool successful = true;
    std::error_code write_error;
    std::size_t offset = 0;
    while (offset < content.size()) {
      if (stop_token.stop_requested()) {
        successful = false;
        break;
      }
      DWORD written = 0;
      const auto count =
        static_cast<DWORD>((std::min<std::size_t>)(content.size() - offset, 1024U * 1024U));
      if (!WriteFile(handle, content.data() + offset, count, &written, nullptr) || written == 0) {
        write_error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        successful = false;
        break;
      }
      offset += written;
    }
    if (successful && !FlushFileBuffers(handle)) {
      write_error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
      successful = false;
    }
    CloseHandle(handle);
#else
    const auto descriptor =
      ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (descriptor < 0) {
      if (errno == EEXIST)
        continue;
      const std::error_code error(errno, std::generic_category());
      return failure(status_from_error(error),
        destination,
        "failed to create temporary file: " + error.message());
    }
    bool successful = true;
    std::error_code write_error;
    std::size_t offset = 0;
    while (offset < content.size()) {
      if (stop_token.stop_requested()) {
        successful = false;
        break;
      }
      const auto count = ::write(descriptor, content.data() + offset, content.size() - offset);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0) {
        write_error = std::error_code(errno, std::generic_category());
        successful = false;
        break;
      }
      offset += static_cast<std::size_t>(count);
    }
    if (successful && ::fsync(descriptor) != 0) {
      write_error = std::error_code(errno, std::generic_category());
      successful = false;
    }
    if (::close(descriptor) != 0 && successful) {
      write_error = std::error_code(errno, std::generic_category());
      successful = false;
    }
#endif
    if (successful) {
      return { .status = filesystem_status::ok, .path = destination };
    }
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    temporary.clear();
    if (stop_token.stop_requested()) {
      return failure(filesystem_status::cancelled, destination, "operation cancelled");
    }
    return failure(filesystem_status::io_error,
      destination,
      write_error ? "failed to write temporary file: " + write_error.message()
                  : "failed to write temporary file");
  }
  return failure(
    filesystem_status::io_error, destination, "failed to allocate a unique temporary file name");
}

bool atomic_replace(const std::filesystem::path& source, const std::filesystem::path& destination,
  std::error_code& error) {
#ifdef _WIN32
  if (MoveFileExW(
        source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
  error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
  return false;
#else
  std::filesystem::rename(source, destination, error);
  return !error;
#endif
}

bool atomic_create(const std::filesystem::path& source, const std::filesystem::path& destination,
  std::error_code& error) {
#ifdef _WIN32
  if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
  error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
  return false;
#else
  if (::link(source.c_str(), destination.c_str()) == 0)
    return true;
  error = std::error_code(errno, std::generic_category());
  return false;
#endif
}

filesystem_result write_file_atomically(
  const write_text_request& request, std::stop_token stop_token) {
  if (!valid_utf8(request.content)) {
    return failure(filesystem_status::type_mismatch,
      request.path,
      "content must be valid UTF-8 text without NUL bytes");
  }
  if (stop_token.stop_requested()) {
    return failure(filesystem_status::cancelled, request.path, "operation cancelled");
  }
  std::error_code error;
  const auto exists = std::filesystem::exists(request.path, error);
  if (error)
    return failure(status_from_error(error), request.path, error.message());
  std::filesystem::file_status existing_status;
  if (exists) {
    existing_status = std::filesystem::symlink_status(request.path, error);
    if (error)
      return failure(status_from_error(error), request.path, error.message());
    if (std::filesystem::is_symlink(existing_status)) {
      return failure(filesystem_status::permission_denied,
        request.path,
        "writing through a symbolic link is not supported");
    }
    if (!std::filesystem::is_regular_file(existing_status)) {
      return failure(
        filesystem_status::type_mismatch, request.path, "destination is not a regular file");
    }
  }
  if (request.disposition == write_disposition::create_new && exists) {
    return failure(filesystem_status::already_exists, request.path, "file already exists");
  }
  if (request.expected_revision) {
    if (!exists) {
      return failure(filesystem_status::conflict,
        request.path,
        "expected revision was supplied but the file does not exist");
    }
    const auto current = read_regular_file(request.path, 0, stop_token);
    if (!current.successful())
      return current;
    if (current.revision != *request.expected_revision) {
      return failure(filesystem_status::conflict,
        request.path,
        "file revision does not match expected_revision");
    }
  }
  bool parent_directories_created = false;
  if (request.create_parent_directories && !request.path.parent_path().empty()) {
    const auto parent_existed = std::filesystem::exists(request.path.parent_path(), error);
    if (error)
      return failure(status_from_error(error), request.path, error.message());
    parent_directories_created =
      std::filesystem::create_directories(request.path.parent_path(), error);
    if (error) {
      auto result = failure(status_from_error(error), request.path, error.message());
      if (!parent_existed) {
        result.metadata["parent_directories_created"] = "true";
        result.metadata["partial"] = "true";
      }
      return result;
    }
  }
  const auto with_parent_progress = [&](filesystem_result result) {
    if (parent_directories_created && !result.successful()) {
      result.metadata["parent_directories_created"] = "true";
      result.metadata["partial"] = "true";
    }
    return result;
  };
  std::filesystem::path temporary;
  auto prepared = create_temporary_file(request.path, request.content, stop_token, temporary);
  if (!prepared.successful())
    return with_parent_progress(std::move(prepared));
  struct cleanup {
    std::filesystem::path path;
    ~cleanup() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } cleanup_guard { temporary };
  if (exists) {
    std::filesystem::permissions(
      temporary, existing_status.permissions(), std::filesystem::perm_options::replace, error);
    if (error) {
      return with_parent_progress(failure(status_from_error(error),
        request.path,
        "failed to preserve destination permissions: " + error.message()));
    }
  }
  if (stop_token.stop_requested()) {
    return with_parent_progress(
      failure(filesystem_status::cancelled, request.path, "operation cancelled"));
  }
  if (request.disposition == write_disposition::create_new) {
    if (!atomic_create(temporary, request.path, error)) {
      return with_parent_progress(failure(status_from_error(error), request.path, error.message()));
    }
  }
  else if (!atomic_replace(temporary, request.path, error)) {
    return with_parent_progress(failure(status_from_error(error), request.path, error.message()));
  }
  return {
    .status = filesystem_status::ok,
    .path = request.path,
    .revision = revision_for(request.content),
    .bytes_processed = request.content.size(),
    .affected_items = 1,
  };
}

filesystem_entry_type entry_type_for(const std::filesystem::file_status& status) {
  if (std::filesystem::is_symlink(status))
    return filesystem_entry_type::symlink;
  if (std::filesystem::is_regular_file(status))
    return filesystem_entry_type::regular_file;
  if (std::filesystem::is_directory(status))
    return filesystem_entry_type::directory;
  return filesystem_entry_type::other;
}

filesystem_entry make_entry(const std::filesystem::directory_entry& entry) {
  std::error_code error;
  const auto status = entry.symlink_status(error);
  filesystem_entry result { .path = entry.path() };
  if (error)
    return result;
  result.type = entry_type_for(status);
  if (result.type == filesystem_entry_type::regular_file) {
    result.size = entry.file_size(error);
  }
  return result;
}

std::regex glob_regex(std::string_view pattern) {
  std::string expression = "^";
  for (std::size_t i = 0; i < pattern.size(); ++i) {
    const auto ch = pattern[i];
    if (ch == '*') {
      if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
        if (i + 2 < pattern.size() && (pattern[i + 2] == '/' || pattern[i + 2] == '\\')) {
          expression += "(?:.*/)?";
          i += 2;
        }
        else {
          expression += ".*";
          ++i;
        }
      }
      else
        expression += "[^/]*";
    }
    else if (ch == '?')
      expression += "[^/]";
    else {
      if (std::string_view(".^$|()[]{}+\\").find(ch) != std::string_view::npos) {
        expression.push_back('\\');
      }
      expression.push_back(ch == '\\' ? '/' : ch);
    }
  }
  expression += '$';
  return std::regex(expression, std::regex::ECMAScript);
}

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

} // namespace

filesystem_result local_filesystem_backend::read_text(
  const read_text_request& request, std::stop_token stop_token) {
  return read_regular_file(request.path, request.max_bytes, stop_token);
}

filesystem_result local_filesystem_backend::file_info(
  const file_info_request& request, std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    return failure(filesystem_status::cancelled, request.path, "operation cancelled");
  }
  std::error_code error;
  const auto status = std::filesystem::symlink_status(request.path, error);
  if (error)
    return failure(status_from_error(error), request.path, error.message());
  if (!std::filesystem::exists(status)) {
    return failure(filesystem_status::not_found, request.path, "path does not exist");
  }
  filesystem_result result {
    .status = filesystem_status::ok,
    .path = request.path,
    .affected_items = 1,
  };
  result.metadata["type"] = to_string(entry_type_for(status));
  if (std::filesystem::is_regular_file(status)) {
    const auto size = std::filesystem::file_size(request.path, error);
    if (error)
      return failure(status_from_error(error), request.path, error.message());
    result.bytes_processed = static_cast<std::size_t>(size);
    result.metadata["size"] = std::to_string(size);
    if (request.include_revision) {
      auto file = read_regular_file(request.path, request.max_revision_bytes, stop_token, false);
      if (!file.successful())
        return file;
      result.revision = std::move(file.revision);
    }
  }
  return result;
}

filesystem_result local_filesystem_backend::write_text(
  const write_text_request& request, std::stop_token stop_token) {
  std::scoped_lock lock(mutation_mutex_);
  return write_file_atomically(request, stop_token);
}

filesystem_result local_filesystem_backend::replace_text(
  const replace_text_request& request, std::stop_token stop_token) {
  std::scoped_lock lock(mutation_mutex_);
  if (request.old_text.empty()) {
    return failure(filesystem_status::invalid_request, request.path, "old_text must not be empty");
  }
  auto current = read_regular_file(request.path, request.max_result_bytes, stop_token);
  if (!current.successful())
    return current;
  if (request.expected_revision && current.revision != *request.expected_revision) {
    return failure(
      filesystem_status::conflict, request.path, "file revision does not match expected_revision");
  }
  std::size_t occurrences = 0;
  for (std::size_t pos = 0;
       (pos = current.content.find(request.old_text, pos)) != std::string::npos;
       pos += request.old_text.size()) {
    ++occurrences;
  }
  const auto required = request.replace_all ? occurrences : request.expected_replacements;
  if (occurrences == 0 || (!request.replace_all && occurrences != request.expected_replacements)) {
    return failure(filesystem_status::conflict,
      request.path,
      "old_text occurrence count does not match the request");
  }
  std::string updated;
  updated.reserve(current.content.size());
  std::size_t cursor = 0;
  std::size_t replaced = 0;
  while (replaced < required) {
    const auto found = current.content.find(request.old_text, cursor);
    if (found == std::string::npos)
      break;
    updated.append(current.content, cursor, found - cursor);
    updated += request.new_text;
    cursor = found + request.old_text.size();
    ++replaced;
  }
  updated.append(current.content, cursor);
  if (request.max_result_bytes > 0 && updated.size() > request.max_result_bytes) {
    return failure(filesystem_status::limit_exceeded,
      request.path,
      "replacement result exceeds max_result_bytes");
  }
  auto result = write_file_atomically(
    {
      .path = request.path,
      .content = std::move(updated),
      .disposition = write_disposition::overwrite,
      .expected_revision = current.revision,
    },
    stop_token);
  result.affected_items = replaced;
  return result;
}

filesystem_result local_filesystem_backend::list_directory(
  const list_directory_request& request, std::stop_token stop_token) {
  if (request.max_entries == 0) {
    return failure(
      filesystem_status::limit_exceeded, request.path, "max_entries must be greater than zero");
  }
  std::error_code error;
  if (!std::filesystem::is_directory(request.path, error)) {
    return failure(error ? status_from_error(error) : filesystem_status::type_mismatch,
      request.path,
      error ? error.message() : "path is not a directory");
  }
  filesystem_result result { .status = filesystem_status::ok, .path = request.path };
  const auto options = std::filesystem::directory_options::none;
  if (!request.recursive) {
    for (std::filesystem::directory_iterator iterator(request.path, options, error), end;
         iterator != end && !error;
         iterator.increment(error)) {
      if (stop_token.stop_requested())
        return failure(filesystem_status::cancelled, request.path, "operation cancelled");
      if (result.entries.size() >= request.max_entries) {
        result.truncated = true;
        break;
      }
      result.entries.push_back(make_entry(*iterator));
    }
  }
  else {
    for (std::filesystem::recursive_directory_iterator iterator(request.path, options, error), end;
         iterator != end && !error;
         iterator.increment(error)) {
      if (stop_token.stop_requested())
        return failure(filesystem_status::cancelled, request.path, "operation cancelled");
      if (iterator.depth() >= static_cast<int>(request.max_depth) &&
          iterator->is_directory(error)) {
        iterator.disable_recursion_pending();
      }
      if (result.entries.size() >= request.max_entries) {
        result.truncated = true;
        break;
      }
      result.entries.push_back(make_entry(*iterator));
    }
  }
  if (error)
    return failure(status_from_error(error), request.path, error.message());
  result.affected_items = result.entries.size();
  std::sort(result.entries.begin(), result.entries.end(), [](const auto& left, const auto& right) {
    return left.path.generic_string() < right.path.generic_string();
  });
  return result;
}

filesystem_result local_filesystem_backend::glob(
  const glob_request& request, std::stop_token stop_token) {
  if (request.pattern.empty()) {
    return failure(
      filesystem_status::invalid_request, request.path, "glob pattern must not be empty");
  }
  std::regex pattern;
  try {
    pattern = glob_regex(request.pattern);
  }
  catch (const std::regex_error& error) {
    return failure(filesystem_status::invalid_request, request.path, error.what());
  }
  auto listed = list_directory(
    {
      .path = request.path,
      .recursive = true,
      .max_depth = request.max_depth,
      .max_entries = request.max_entries,
    },
    stop_token);
  if (!listed.successful())
    return listed;
  std::vector<filesystem_entry> matches;
  for (auto& entry : listed.entries) {
    std::error_code error;
    const auto relative = std::filesystem::relative(entry.path, request.path, error);
    if (!error && std::regex_match(relative.generic_string(), pattern)) {
      matches.push_back(std::move(entry));
    }
  }
  listed.entries = std::move(matches);
  listed.affected_items = listed.entries.size();
  return listed;
}

filesystem_result local_filesystem_backend::search_text(
  const search_text_request& request, std::stop_token stop_token) {
  if (request.query.empty()) {
    return failure(filesystem_status::invalid_request, request.path, "query must not be empty");
  }
  if (request.file_pattern.empty()) {
    return failure(
      filesystem_status::invalid_request, request.path, "file_pattern must not be empty");
  }
  if (request.max_files == 0 || request.max_results == 0 || request.max_output_bytes == 0) {
    return failure(
      filesystem_status::limit_exceeded, request.path, "search limits must be greater than zero");
  }
  auto files = glob(
    {
      .path = request.path,
      .pattern = request.file_pattern,
      .max_depth = request.max_depth,
      .max_entries = request.max_files,
    },
    stop_token);
  if (!files.successful())
    return files;
  filesystem_result result { .status = filesystem_status::ok, .path = request.path };
  const auto enumeration_truncated = files.truncated;
  const auto needle = request.case_sensitive ? request.query : lower_ascii(request.query);
  std::size_t total_bytes = 0;
  std::size_t skipped_binary = 0;
  std::size_t skipped_large = 0;
  std::size_t skipped_errors = 0;
  std::size_t output_bytes = 0;
  bool result_limit_reached = false;
  for (const auto& entry : files.entries) {
    if (entry.type != filesystem_entry_type::regular_file)
      continue;
    if (stop_token.stop_requested())
      return failure(filesystem_status::cancelled, request.path, "operation cancelled");
    if (request.max_file_bytes > 0 && entry.size > request.max_file_bytes) {
      ++skipped_large;
      continue;
    }
    if (request.max_total_bytes > 0 &&
        (entry.size > request.max_total_bytes ||
          total_bytes > request.max_total_bytes - static_cast<std::size_t>(entry.size))) {
      result.truncated = true;
      break;
    }
    auto file = read_regular_file(entry.path, request.max_file_bytes, stop_token);
    if (file.status == filesystem_status::type_mismatch) {
      ++skipped_binary;
      continue;
    }
    if (!file.successful()) {
      ++skipped_errors;
      continue;
    }
    total_bytes += file.content.size();
    std::istringstream lines(file.content);
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(lines, line)) {
      ++line_number;
      const auto haystack = request.case_sensitive ? line : lower_ascii(line);
      std::size_t offset = 0;
      while ((offset = haystack.find(needle, offset)) != std::string::npos) {
        if (line.size() > request.max_output_bytes - output_bytes) {
          result_limit_reached = true;
          result.metadata["output_limit_reached"] = "true";
          break;
        }
        result.matches.push_back({
          .path = entry.path,
          .line = line_number,
          .column = offset + 1,
          .text = line,
        });
        output_bytes += line.size();
        if (result.matches.size() >= request.max_results) {
          result_limit_reached = true;
          break;
        }
        offset += needle.size();
      }
      if (result_limit_reached)
        break;
    }
    if (result_limit_reached)
      break;
  }
  result.truncated = result.truncated || result_limit_reached || enumeration_truncated;
  result.bytes_processed = total_bytes;
  result.affected_items = result.matches.size();
  result.metadata["skipped_binary_files"] = std::to_string(skipped_binary);
  result.metadata["skipped_large_files"] = std::to_string(skipped_large);
  result.metadata["skipped_error_files"] = std::to_string(skipped_errors);
  result.metadata["file_enumeration_truncated"] = enumeration_truncated ? "true" : "false";
  result.metadata["output_bytes"] = std::to_string(output_bytes);
  return result;
}

filesystem_result local_filesystem_backend::create_directory(
  const create_directory_request& request, std::stop_token stop_token) {
  std::scoped_lock lock(mutation_mutex_);
  if (stop_token.stop_requested())
    return failure(filesystem_status::cancelled, request.path, "operation cancelled");
  std::error_code error;
  const auto existed = std::filesystem::exists(request.path, error);
  if (error)
    return failure(status_from_error(error), request.path, error.message());
  const auto created = request.recursive ? std::filesystem::create_directories(request.path, error)
                                         : std::filesystem::create_directory(request.path, error);
  if (error) {
    auto result = failure(status_from_error(error), request.path, error.message());
    if (request.recursive && !existed) {
      result.metadata["partial"] = "true";
      result.metadata["parent_directories_created"] = "true";
    }
    return result;
  }
  return {
    .status = filesystem_status::ok,
    .path = request.path,
    .affected_items = created ? 1U : 0U,
    .metadata = { { "partial", "false" } },
  };
}

filesystem_result local_filesystem_backend::copy_path(
  const transfer_path_request& request, std::stop_token stop_token) {
  std::scoped_lock lock(mutation_mutex_);
  if (stop_token.stop_requested()) {
    return failure(filesystem_status::cancelled, request.source, "operation cancelled");
  }
  std::error_code error;
  const auto source_status = std::filesystem::symlink_status(request.source, error);
  if (error)
    return failure(status_from_error(error), request.source, error.message());
  if (!std::filesystem::exists(source_status)) {
    return failure(filesystem_status::not_found, request.source, "source path does not exist");
  }
  if (std::filesystem::is_symlink(source_status)) {
    return failure(filesystem_status::permission_denied,
      request.source,
      "copying symbolic links is not supported");
  }
  const auto destination_exists = std::filesystem::exists(request.destination, error);
  if (error) {
    return failure(status_from_error(error),
      request.destination,
      "failed to inspect copy destination: " + error.message());
  }
  std::filesystem::path offending_symlink;
  if (existing_path_contains_symlink(
        request.destination, request.destination, offending_symlink, error)) {
    return failure(filesystem_status::permission_denied,
      offending_symlink,
      "copy destination contains a symbolic link");
  }
  if (error) {
    return failure(status_from_error(error),
      request.destination,
      "failed to inspect copy destination components: " + error.message());
  }
  if (destination_exists) {
    const auto normalized_source = normalized_for_comparison(request.source, error);
    if (error)
      return failure(status_from_error(error), request.source, error.message());
    const auto normalized_destination = normalized_for_comparison(request.destination, error);
    if (error) {
      return failure(status_from_error(error), request.destination, error.message());
    }
    if (path_within(normalized_source, normalized_destination) &&
        path_within(normalized_destination, normalized_source)) {
      return failure(
        filesystem_status::invalid_path, request.source, "source and destination must differ");
    }
  }
  if (destination_exists && !request.overwrite) {
    return failure(
      filesystem_status::already_exists, request.destination, "destination already exists");
  }
  filesystem_result result {
    .status = filesystem_status::ok, .path = request.source, .destination = request.destination
  };
  if (std::filesystem::is_regular_file(source_status)) {
    const auto size = std::filesystem::file_size(request.source, error);
    if (error)
      return failure(status_from_error(error), request.source, error.message());
    if (request.max_bytes > 0 && size > request.max_bytes) {
      return failure(filesystem_status::limit_exceeded, request.source, "copy exceeds max_bytes");
    }
    if (!request.destination.parent_path().empty()) {
      const auto parents_existed =
        std::filesystem::exists(request.destination.parent_path(), error);
      if (error) {
        return failure(status_from_error(error), request.destination, error.message());
      }
      const auto parents_created =
        std::filesystem::create_directories(request.destination.parent_path(), error);
      if (parents_created || (error && !parents_existed)) {
        result.metadata["parent_directories_created"] = "true";
      }
      if (error) {
        return failure_with_progress(
          std::move(result), status_from_error(error), request.destination, error.message());
      }
    }
    const auto options = request.overwrite ? std::filesystem::copy_options::overwrite_existing
                                           : std::filesystem::copy_options::none;
    const auto copied =
      std::filesystem::copy_file(request.source, request.destination, options, error);
    if (!copied) {
      if (error) {
        return failure_with_progress(
          std::move(result), status_from_error(error), request.destination, error.message());
      }
      return failure_with_progress(std::move(result),
        filesystem_status::already_exists,
        request.destination,
        "destination was not copied");
    }
    result.affected_items = 1;
    result.bytes_processed = static_cast<std::size_t>(size);
    result.metadata["partial"] = "false";
    return result;
  }
  if (!std::filesystem::is_directory(source_status) || !request.recursive) {
    return failure(filesystem_status::type_mismatch,
      request.source,
      "recursive=true is required to copy a directory");
  }
  if (request.max_entries == 0) {
    return failure(filesystem_status::limit_exceeded,
      request.source,
      "max_entries must be greater than zero for a recursive copy");
  }
  const auto normalized_source = normalized_for_comparison(request.source, error);
  if (error)
    return failure(status_from_error(error), request.source, error.message());
  const auto normalized_destination = normalized_for_comparison(request.destination, error);
  if (error)
    return failure(status_from_error(error),
      request.destination,
      "failed to normalize copy destination: " + error.message());
  if (path_within(normalized_destination, normalized_source)) {
    return failure(filesystem_status::invalid_path,
      request.destination,
      "a directory cannot be copied into itself");
  }

  struct copy_entry {
    std::filesystem::path source;
    std::filesystem::path destination;
    filesystem_entry_type type { filesystem_entry_type::other };
    std::size_t size { 0 };
  };
  std::vector<copy_entry> plan;
  plan.reserve((std::min<std::size_t>)(request.max_entries, 1024));
  std::size_t planned_bytes = 0;
  error.clear();
  const auto options = std::filesystem::directory_options::none;
  for (std::filesystem::recursive_directory_iterator iterator(request.source, options, error), end;
       iterator != end && !error;
       iterator.increment(error)) {
    if (stop_token.stop_requested()) {
      return failure(filesystem_status::cancelled, request.source, "operation cancelled");
    }
    if (plan.size() >= request.max_entries) {
      return failure(
        filesystem_status::limit_exceeded, request.source, "copy entry limit exceeded");
    }
    const auto relative = std::filesystem::relative(iterator->path(), request.source, error);
    if (error)
      break;
    const auto target = request.destination / relative;
    const auto status = iterator->symlink_status(error);
    if (error)
      break;
    if (std::filesystem::is_symlink(status)) {
      return failure(filesystem_status::permission_denied,
        iterator->path(),
        "copying symbolic links is not supported");
    }
    if (std::filesystem::is_directory(status)) {
      plan.push_back({
        .source = iterator->path(),
        .destination = target,
        .type = filesystem_entry_type::directory,
      });
    }
    else if (std::filesystem::is_regular_file(status)) {
      const auto size = iterator->file_size(error);
      if (error)
        break;
      if (request.max_bytes > 0 &&
          (size > request.max_bytes ||
            planned_bytes > request.max_bytes - static_cast<std::size_t>(size))) {
        return failure(
          filesystem_status::limit_exceeded, iterator->path(), "copy exceeds max_bytes");
      }
      planned_bytes += static_cast<std::size_t>(size);
      plan.push_back({
        .source = iterator->path(),
        .destination = target,
        .type = filesystem_entry_type::regular_file,
        .size = static_cast<std::size_t>(size),
      });
    }
    else {
      return failure(filesystem_status::type_mismatch,
        iterator->path(),
        "copying special filesystem entries is not supported");
    }
  }
  if (error)
    return failure(status_from_error(error), request.source, error.message());

  for (const auto& entry : plan) {
    offending_symlink.clear();
    if (existing_path_contains_symlink(
          request.destination, entry.destination, offending_symlink, error)) {
      return failure(filesystem_status::permission_denied,
        offending_symlink,
        "copy destination contains a symbolic link");
    }
    if (error) {
      return failure(status_from_error(error), entry.destination, error.message());
    }
  }

  const auto destination_created = !destination_exists;
  std::filesystem::create_directories(request.destination, error);
  if (error) {
    if (!destination_exists)
      result.metadata["destination_created"] = "true";
    return failure_with_progress(
      std::move(result), status_from_error(error), request.destination, error.message());
  }
  if (destination_created)
    result.metadata["destination_created"] = "true";
  for (const auto& entry : plan) {
    if (stop_token.stop_requested()) {
      return failure_with_progress(std::move(result),
        filesystem_status::cancelled,
        entry.source,
        "operation cancelled during copy");
    }
    offending_symlink.clear();
    if (existing_path_contains_symlink(
          request.destination, entry.destination, offending_symlink, error)) {
      return failure_with_progress(std::move(result),
        filesystem_status::permission_denied,
        offending_symlink,
        "copy destination contains a symbolic link");
    }
    if (error) {
      return failure_with_progress(
        std::move(result), status_from_error(error), entry.destination, error.message());
    }
    if (entry.type == filesystem_entry_type::directory) {
      std::filesystem::create_directories(entry.destination, error);
    }
    else {
      std::filesystem::create_directories(entry.destination.parent_path(), error);
      if (!error) {
        const auto copied = std::filesystem::copy_file(entry.source,
          entry.destination,
          request.overwrite ? std::filesystem::copy_options::overwrite_existing
                            : std::filesystem::copy_options::none,
          error);
        if (!copied && !error) {
          error = std::make_error_code(std::errc::file_exists);
        }
      }
      if (!error)
        result.bytes_processed += entry.size;
    }
    if (error) {
      return failure_with_progress(
        std::move(result), status_from_error(error), entry.destination, error.message());
    }
    ++result.affected_items;
  }
  result.metadata["partial"] = "false";
  return result;
}

filesystem_result local_filesystem_backend::move_path(
  const transfer_path_request& request, std::stop_token stop_token) {
  std::scoped_lock lock(mutation_mutex_);
  if (stop_token.stop_requested())
    return failure(filesystem_status::cancelled, request.source, "operation cancelled");
  std::error_code error;
  filesystem_result result {
    .status = filesystem_status::ok,
    .path = request.source,
    .destination = request.destination,
  };
  if (request.source == request.destination) {
    return failure(
      filesystem_status::invalid_path, request.source, "source and destination must differ");
  }
  const auto source_status = std::filesystem::symlink_status(request.source, error);
  if (error)
    return failure(status_from_error(error), request.source, error.message());
  if (!std::filesystem::exists(source_status)) {
    return failure(filesystem_status::not_found, request.source, "source path does not exist");
  }
  if (std::filesystem::is_symlink(source_status)) {
    return failure(filesystem_status::permission_denied,
      request.source,
      "moving symbolic links is not supported");
  }
  if (std::filesystem::is_directory(source_status) && !request.recursive) {
    return failure(filesystem_status::type_mismatch,
      request.source,
      "recursive=true is required to move a directory");
  }
  if (std::filesystem::is_directory(source_status)) {
    const auto normalized_source = normalized_for_comparison(request.source, error);
    if (error)
      return failure(status_from_error(error), request.source, error.message());
    const auto normalized_destination = normalized_for_comparison(request.destination, error);
    if (error) {
      return failure(status_from_error(error), request.destination, error.message());
    }
    if (path_within(normalized_destination, normalized_source)) {
      return failure(filesystem_status::invalid_path,
        request.destination,
        "a directory cannot be moved into itself");
    }
  }
  const auto destination_exists = std::filesystem::exists(request.destination, error);
  if (error) {
    return failure(status_from_error(error), request.destination, error.message());
  }
  if (!request.overwrite && destination_exists) {
    return failure(
      filesystem_status::already_exists, request.destination, "destination already exists");
  }
  if (!request.destination.parent_path().empty()) {
    const auto parents_existed = std::filesystem::exists(request.destination.parent_path(), error);
    if (error) {
      return failure(status_from_error(error), request.destination, error.message());
    }
    const auto parents_created =
      std::filesystem::create_directories(request.destination.parent_path(), error);
    if (parents_created || (error && !parents_existed)) {
      result.metadata["parent_directories_created"] = "true";
    }
    if (error) {
      return failure_with_progress(
        std::move(result), status_from_error(error), request.destination, error.message());
    }
  }
  if (destination_exists) {
    const auto destination_status = std::filesystem::symlink_status(request.destination, error);
    if (error) {
      return failure(status_from_error(error), request.destination, error.message());
    }
    if (!std::filesystem::is_regular_file(source_status) ||
        !std::filesystem::is_regular_file(destination_status)) {
      return failure(filesystem_status::type_mismatch,
        request.destination,
        "overwrite is supported only when both source and destination are regular files");
    }
    if (!atomic_replace(request.source, request.destination, error)) {
      return failure_with_progress(
        std::move(result), status_from_error(error), request.source, error.message());
    }
  }
  else {
    std::filesystem::rename(request.source, request.destination, error);
  }
  if (error) {
    return failure_with_progress(
      std::move(result), status_from_error(error), request.source, error.message());
  }
  result.affected_items = 1;
  result.metadata["partial"] = "false";
  return result;
}

filesystem_result local_filesystem_backend::remove_path(
  const remove_path_request& request, std::stop_token stop_token) {
  std::scoped_lock lock(mutation_mutex_);
  if (stop_token.stop_requested())
    return failure(filesystem_status::cancelled, request.path, "operation cancelled");
  std::error_code error;
  const auto status = std::filesystem::symlink_status(request.path, error);
  if (error)
    return failure(status_from_error(error), request.path, error.message());
  if (!std::filesystem::exists(status))
    return failure(filesystem_status::not_found, request.path, "path does not exist");
  if (request.expected_revision) {
    auto current = read_regular_file(request.path, 0, stop_token);
    if (!current.successful())
      return current;
    if (current.revision != *request.expected_revision)
      return failure(filesystem_status::conflict,
        request.path,
        "file revision does not match expected_revision");
  }
  filesystem_result result { .status = filesystem_status::ok, .path = request.path };
  if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
    if (!std::filesystem::remove(request.path, error) || error)
      return failure(status_from_error(error), request.path, error.message());
    result.affected_items = 1;
    return result;
  }
  if (!request.recursive) {
    if (!std::filesystem::remove(request.path, error) || error)
      return failure(
        status_from_error(error), request.path, error ? error.message() : "directory is not empty");
    result.affected_items = 1;
    return result;
  }
  std::vector<std::filesystem::path> paths;
  const auto options = std::filesystem::directory_options::none;
  for (std::filesystem::recursive_directory_iterator iterator(request.path, options, error), end;
       iterator != end && !error;
       iterator.increment(error)) {
    if (stop_token.stop_requested())
      return failure(filesystem_status::cancelled, request.path, "operation cancelled");
    if (paths.size() >= request.max_entries)
      return failure(
        filesystem_status::limit_exceeded, request.path, "remove entry limit exceeded");
    paths.push_back(iterator->path());
  }
  if (error)
    return failure(status_from_error(error), request.path, error.message());
  std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right) {
    return std::distance(left.begin(), left.end()) > std::distance(right.begin(), right.end());
  });
  for (const auto& path : paths) {
    if (stop_token.stop_requested()) {
      return failure_with_progress(std::move(result),
        filesystem_status::cancelled,
        request.path,
        "operation cancelled during removal");
    }
    if (std::filesystem::remove(path, error))
      ++result.affected_items;
    if (error) {
      return failure_with_progress(
        std::move(result), status_from_error(error), path, error.message());
    }
  }
  if (std::filesystem::remove(request.path, error))
    ++result.affected_items;
  if (error) {
    return failure_with_progress(
      std::move(result), status_from_error(error), request.path, error.message());
  }
  result.metadata["partial"] = "false";
  return result;
}

std::unique_ptr<filesystem_backend> make_local_filesystem_backend() {
  return std::make_unique<local_filesystem_backend>();
}

} // namespace wuwe::agent::filesystem
