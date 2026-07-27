#ifndef WUWE_AGENT_FILESYSTEM_FILESYSTEM_POLICY_HPP
#define WUWE_AGENT_FILESYSTEM_FILESYSTEM_POLICY_HPP

#include <cstddef>
#include <filesystem>

namespace wuwe::agent::filesystem {

struct filesystem_policy {
  std::filesystem::path root;
  bool allow_absolute_paths { false };
  bool follow_symlinks { false };
  bool allow_read { true };
  bool allow_write { false };
  bool allow_create_directory { false };
  bool allow_copy { false };
  bool allow_move { false };
  bool allow_remove { false };
  bool require_approval_for_write { true };
  bool require_approval_for_move { true };
  bool require_approval_for_remove { true };
  std::size_t max_read_bytes { 4 * 1024 * 1024 };
  std::size_t max_write_bytes { 4 * 1024 * 1024 };
  std::size_t max_search_file_bytes { 2 * 1024 * 1024 };
  std::size_t max_search_total_bytes { 64 * 1024 * 1024 };
  std::size_t max_search_output_bytes { 4 * 1024 * 1024 };
  std::size_t max_copy_bytes { 64 * 1024 * 1024 };
  std::size_t max_directory_entries { 10000 };
  std::size_t max_search_results { 2000 };
  std::size_t max_search_depth { 64 };
  std::size_t max_pattern_bytes { 4096 };
};

} // namespace wuwe::agent::filesystem

#endif // WUWE_AGENT_FILESYSTEM_FILESYSTEM_POLICY_HPP
