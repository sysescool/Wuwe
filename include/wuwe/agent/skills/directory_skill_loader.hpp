#ifndef WUWE_AGENT_SKILLS_DIRECTORY_SKILL_LOADER_HPP
#define WUWE_AGENT_SKILLS_DIRECTORY_SKILL_LOADER_HPP

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <wuwe/agent/core/content.hpp>
#include <wuwe/agent/skills/skill_observability.hpp>
#include <wuwe/agent/skills/skill_package.hpp>

namespace wuwe::agent::skills {

enum class skill_load_error_code {
  none,
  invalid_options,
  invalid_path,
  root_not_found,
  root_not_directory,
  path_outside_root,
  filesystem_error,
  reparse_point,
  hard_link,
  case_collision,
  manifest_missing,
  manifest_too_large,
  manifest_invalid,
  resource_count_exceeded,
  duplicate_resource_path,
  undeclared_resource,
  resource_missing,
  resource_not_regular,
  resource_too_large,
  package_too_large,
  resource_size_mismatch,
  resource_digest_mismatch,
  package_invalid,
};

[[nodiscard]] const char* to_string(skill_load_error_code value) noexcept;

struct directory_skill_loader_options {
  std::filesystem::path root;
  std::size_t max_manifest_bytes { 256 * 1024 };
  std::size_t max_resource_bytes { 1024 * 1024 };
  std::size_t max_package_bytes { 8 * 1024 * 1024 };
  std::size_t max_resources { 128 };
  core::content_trust_level trust { core::content_trust_level::retrieved_untrusted };
  bool reject_hard_links { true };
};

struct skill_load_result {
  skill_package_ptr package;
  skill_load_error_code error { skill_load_error_code::none };
  std::string message;
  std::vector<skill_diagnostic> diagnostics;

  [[nodiscard]] explicit operator bool() const noexcept {
    return package != nullptr && error == skill_load_error_code::none;
  }
};

class directory_skill_loader final {
public:
  explicit directory_skill_loader(directory_skill_loader_options options);

  [[nodiscard]] const directory_skill_loader_options& options() const noexcept {
    return options_;
  }

  [[nodiscard]] skill_load_result load(const std::filesystem::path& relative_path,
    observability::event_sink* event_sink = nullptr,
    const skill_observability_context& context = {}) const noexcept;

private:
  directory_skill_loader_options options_;
};

} // namespace wuwe::agent::skills

#endif // WUWE_AGENT_SKILLS_DIRECTORY_SKILL_LOADER_HPP
