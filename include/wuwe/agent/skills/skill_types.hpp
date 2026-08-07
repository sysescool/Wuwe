#ifndef WUWE_AGENT_SKILLS_SKILL_TYPES_HPP
#define WUWE_AGENT_SKILLS_SKILL_TYPES_HPP

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/capability/capability.hpp>
#include <wuwe/agent/core/content.hpp>
#include <wuwe/agent/skills/skill_version.hpp>

namespace wuwe::agent::skills {

enum class skill_error_code {
  none,
  invalid_manifest,
  invalid_package,
  invalid_registration,
  registration_conflict,
};

[[nodiscard]] const char* to_string(skill_error_code value) noexcept;

struct skill_error_info {
  skill_error_code code { skill_error_code::none };
  std::string message;

  [[nodiscard]] explicit operator bool() const noexcept {
    return code != skill_error_code::none;
  }
};

enum class skill_resource_kind {
  instructions,
  prompt,
  schema,
  knowledge,
  script,
  template_,
  asset,
};

[[nodiscard]] std::string to_string(skill_resource_kind value);
[[nodiscard]] skill_resource_kind skill_resource_kind_from_string(const std::string& value);

struct skill_resource_descriptor {
  std::string id;
  std::string path;
  skill_resource_kind kind { skill_resource_kind::asset };
  std::string media_type { "application/octet-stream" };
  std::size_t size { 0 };
  std::string sha256;
  std::map<std::string, std::string> metadata;
};

struct skill_tool_requirement {
  std::string name;
  std::optional<std::string> exact_version;
  bool optional { false };
};

using skill_capability_requirement = capability::capability_requirement;

struct skill_knowledge_requirement {
  std::string source;
  std::map<std::string, std::string> filters;
  std::size_t max_context_chars { 0 };
  bool optional { false };
};

struct skill_dependency {
  std::string id;
  version_requirement version;
  bool optional { false };
};

struct skill_descriptor {
  std::string id;
  std::string name;
  std::string description;
  semantic_version version;
  std::vector<std::string> tags;
  std::vector<std::string> examples;
  std::vector<std::string> input_modes;
  std::vector<std::string> output_modes;
  nlohmann::json input_schema = nlohmann::json::object();
  nlohmann::json output_schema = nlohmann::json::object();
  bool deprecated { false };
  std::map<std::string, std::string> metadata;
};

enum class skill_origin_kind {
  embedded,
  local,
  remote,
};

[[nodiscard]] std::string to_string(skill_origin_kind value);

struct skill_provenance {
  skill_origin_kind origin { skill_origin_kind::embedded };
  std::string source_uri;
  std::string content_sha256;
  core::content_trust_level trust { core::content_trust_level::application_trusted };
  std::map<std::string, std::string> metadata;
};

struct skill_resource {
  skill_resource_descriptor descriptor;
  std::string content;
  std::string sha256;
};

enum class skill_diagnostic_severity {
  information,
  warning,
  error,
};

struct skill_diagnostic {
  skill_diagnostic_severity severity { skill_diagnostic_severity::error };
  std::string code;
  std::string message;
  std::string skill_id;
  std::vector<std::string> dependency_path;
};

} // namespace wuwe::agent::skills

#endif // WUWE_AGENT_SKILLS_SKILL_TYPES_HPP
