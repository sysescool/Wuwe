#ifndef WUWE_AGENT_MULTI_AGENT_SKILLS_ADAPTER_HPP
#define WUWE_AGENT_MULTI_AGENT_SKILLS_ADAPTER_HPP

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

#include <wuwe/agent/multi_agent/multi_agent_core.hpp>
#include <wuwe/agent/skills/skill_package.hpp>

namespace wuwe::agent::multi_agent {

[[nodiscard]] inline agent_skill agent_skill_from_descriptor(
  const skills::skill_descriptor& descriptor) {
  return {
    .id = descriptor.id,
    .name = descriptor.name,
    .description = descriptor.description,
    .tags = descriptor.tags,
    .input_modes = descriptor.input_modes,
    .output_modes = descriptor.output_modes,
    .metadata = {
      { "wuwe.skill.version", descriptor.version.string() },
      { "wuwe.skill.deprecated", descriptor.deprecated ? "true" : "false" },
    },
  };
}

[[nodiscard]] inline agent_skill agent_skill_from_package(const skills::skill_package& package) {
  auto output = agent_skill_from_descriptor(package.descriptor());
  output.metadata["wuwe.skill.digest"] = package.provenance().content_sha256;
  output.metadata["wuwe.skill.origin"] = skills::to_string(package.provenance().origin);
  return output;
}

inline void attach_skill(agent_descriptor& agent, agent_skill skill) {
  if (skill.id.empty()) {
    throw std::invalid_argument("attached agent skill requires an id");
  }
  if (std::any_of(agent.skills.begin(), agent.skills.end(), [&](const auto& existing) {
        return existing.id == skill.id;
      })) {
    throw std::invalid_argument("duplicate attached agent skill: " + skill.id);
  }
  agent.skills.push_back(std::move(skill));
}

inline void attach_skill(agent_descriptor& agent, const skills::skill_package& package) {
  attach_skill(agent, agent_skill_from_package(package));
}

} // namespace wuwe::agent::multi_agent

#endif // WUWE_AGENT_MULTI_AGENT_SKILLS_ADAPTER_HPP
