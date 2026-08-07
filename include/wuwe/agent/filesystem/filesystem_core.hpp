#ifndef WUWE_AGENT_FILESYSTEM_FILESYSTEM_CORE_HPP
#define WUWE_AGENT_FILESYSTEM_FILESYSTEM_CORE_HPP

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wuwe::agent::filesystem {

enum class filesystem_status {
  ok,
  not_found,
  already_exists,
  invalid_path,
  invalid_request,
  outside_root,
  permission_denied,
  approval_denied,
  type_mismatch,
  conflict,
  limit_exceeded,
  cancelled,
  io_error,
};

enum class filesystem_entry_type {
  regular_file,
  directory,
  symlink,
  other,
};

enum class write_disposition {
  create_new,
  overwrite,
};

struct filesystem_entry {
  std::filesystem::path path;
  filesystem_entry_type type { filesystem_entry_type::other };
  std::uintmax_t size { 0 };
  std::string revision;
};

struct text_match {
  std::filesystem::path path;
  std::size_t line { 0 };
  std::size_t column { 0 };
  std::string text;
};

struct filesystem_result {
  filesystem_status status { filesystem_status::io_error };
  std::string error_message;
  std::filesystem::path path;
  std::filesystem::path destination;
  std::string content;
  std::string revision;
  std::size_t bytes_processed { 0 };
  std::size_t affected_items { 0 };
  bool truncated { false };
  std::vector<filesystem_entry> entries;
  std::vector<text_match> matches;
  std::map<std::string, std::string> metadata;

  [[nodiscard]] bool successful() const noexcept {
    return status == filesystem_status::ok;
  }
};

struct read_text_request {
  std::filesystem::path path;
  std::size_t max_bytes { 0 };
  std::map<std::string, std::string> metadata;
};

struct file_info_request {
  std::filesystem::path path;
  bool include_revision { false };
  std::size_t max_revision_bytes { 0 };
  std::map<std::string, std::string> metadata;
};

struct write_text_request {
  std::filesystem::path path;
  std::string content;
  write_disposition disposition { write_disposition::overwrite };
  std::optional<std::string> expected_revision;
  bool create_parent_directories { false };
  std::map<std::string, std::string> metadata;
};

struct replace_text_request {
  std::filesystem::path path;
  std::string old_text;
  std::string new_text;
  std::optional<std::string> expected_revision;
  std::size_t expected_replacements { 1 };
  bool replace_all { false };
  std::size_t max_result_bytes { 0 };
  std::map<std::string, std::string> metadata;
};

struct list_directory_request {
  std::filesystem::path path;
  bool recursive { false };
  std::size_t max_depth { 1 };
  std::size_t max_entries { 1000 };
  std::map<std::string, std::string> metadata;
};

struct glob_request {
  std::filesystem::path path;
  std::string pattern { "*" };
  std::size_t max_depth { 32 };
  std::size_t max_entries { 1000 };
  std::map<std::string, std::string> metadata;
};

struct search_text_request {
  std::filesystem::path path;
  std::string query;
  std::string file_pattern { "**" };
  bool case_sensitive { true };
  std::size_t max_depth { 32 };
  std::size_t max_files { 10000 };
  std::size_t max_results { 200 };
  std::size_t max_file_bytes { 0 };
  std::size_t max_total_bytes { 0 };
  std::size_t max_output_bytes { 0 };
  std::map<std::string, std::string> metadata;
};

struct create_directory_request {
  std::filesystem::path path;
  bool recursive { true };
  std::map<std::string, std::string> metadata;
};

struct transfer_path_request {
  std::filesystem::path source;
  std::filesystem::path destination;
  bool overwrite { false };
  bool recursive { false };
  std::size_t max_entries { 10000 };
  std::size_t max_bytes { 0 };
  std::map<std::string, std::string> metadata;
};

struct remove_path_request {
  std::filesystem::path path;
  bool recursive { false };
  std::size_t max_entries { 10000 };
  std::optional<std::string> expected_revision;
  std::map<std::string, std::string> metadata;
};

[[nodiscard]] std::string to_string(filesystem_status status);
[[nodiscard]] std::string to_string(filesystem_entry_type type);
[[nodiscard]] std::string to_string(write_disposition disposition);

} // namespace wuwe::agent::filesystem

#endif // WUWE_AGENT_FILESYSTEM_FILESYSTEM_CORE_HPP
