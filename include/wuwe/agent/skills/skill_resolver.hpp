#ifndef WUWE_AGENT_SKILLS_SKILL_RESOLVER_HPP
#define WUWE_AGENT_SKILLS_SKILL_RESOLVER_HPP

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include <wuwe/agent/skills/skill_registry.hpp>

namespace wuwe::agent::skills {

struct skill_resolution_limits {
  std::size_t max_roots { 64 };
  std::size_t max_skills { 256 };
  std::size_t max_depth { 64 };
  std::size_t max_dependencies_per_skill { 256 };
  std::size_t max_candidate_attempts { 10000 };
};

struct skill_resolution_request {
  std::vector<skill_dependency> roots;
  skill_resolution_limits limits;
};

struct resolved_skill {
  skill_package_ptr package;
  bool root { false };
};

struct skill_resolution_result {
  bool success { false };
  std::vector<resolved_skill> skills;
  std::vector<skill_diagnostic> diagnostics;

  [[nodiscard]] explicit operator bool() const noexcept {
    return success;
  }
};

class skill_resolver final {
public:
  [[nodiscard]] skill_resolution_result resolve(
    const skill_registry_snapshot& registry, const skill_resolution_request& request) const;
};

} // namespace wuwe::agent::skills

#endif // WUWE_AGENT_SKILLS_SKILL_RESOLVER_HPP
