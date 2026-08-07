#ifndef WUWE_AGENT_SKILLS_SKILL_MANIFEST_HPP
#define WUWE_AGENT_SKILLS_SKILL_MANIFEST_HPP

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/skills/skill_types.hpp>

namespace wuwe::agent::skills {

inline constexpr std::size_t current_skill_manifest_schema_version = 1;

struct skill_manifest {
  std::size_t schema_version { current_skill_manifest_schema_version };
  skill_descriptor descriptor;
  std::vector<std::string> instruction_resources;
  std::vector<skill_resource_descriptor> resources;
  std::vector<skill_tool_requirement> tools;
  std::vector<skill_capability_requirement> capabilities;
  std::vector<skill_knowledge_requirement> knowledge;
  std::vector<skill_dependency> dependencies;
  std::map<std::string, std::string> metadata;
};

struct skill_manifest_limits {
  std::size_t max_manifest_bytes { 1024 * 1024 };
  std::size_t max_string_bytes { 64 * 1024 };
  std::size_t max_collection_items { 4096 };
  std::size_t max_json_depth { 64 };
};

struct skill_manifest_parse_result {
  std::optional<skill_manifest> manifest;
  skill_error_info error;

  [[nodiscard]] explicit operator bool() const noexcept {
    return manifest.has_value() && !error;
  }
};

[[nodiscard]] skill_manifest parse_skill_manifest(
  std::string_view input, const skill_manifest_limits& limits = {});
[[nodiscard]] skill_manifest_parse_result try_parse_skill_manifest(
  std::string_view input, const skill_manifest_limits& limits = {});
[[nodiscard]] skill_manifest skill_manifest_from_json(
  const nlohmann::json& input, const skill_manifest_limits& limits = {});
[[nodiscard]] skill_manifest_parse_result try_skill_manifest_from_json(
  const nlohmann::json& input, const skill_manifest_limits& limits = {});
[[nodiscard]] nlohmann::json skill_manifest_to_json(const skill_manifest& manifest);
void validate_skill_manifest(const skill_manifest& manifest);

} // namespace wuwe::agent::skills

#endif // WUWE_AGENT_SKILLS_SKILL_MANIFEST_HPP
