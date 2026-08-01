#ifndef WUWE_AGENT_SKILLS_PLANNING_ADAPTER_HPP
#define WUWE_AGENT_SKILLS_PLANNING_ADAPTER_HPP

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/core/content.hpp>
#include <wuwe/agent/planning/plan.hpp>
#include <wuwe/agent/skills/skill_activation.hpp>

namespace wuwe::agent::skills {

[[nodiscard]] inline planning::planning_request apply_skill_activation(
  planning::planning_request request, const skill_activation_result& activation) {
  if (!activation) {
    throw std::invalid_argument("cannot apply an unsuccessful skill activation");
  }

  for (const auto& instruction : activation.instructions) {
    const bool trusted = core::trusted_for_system_message(instruction.provenance.trust);
    auto content = instruction.content;
    if (trusted) {
      if (!request.system_prompt.empty()) {
        request.system_prompt += "\n\n";
      }
      request.system_prompt += content;
    }
    else {
      content = core::render_context_boundary(
        "skill:" + instruction.skill_id, instruction.provenance.trust, std::move(content));
      if (!request.input.empty()) {
        request.input += "\n\n";
      }
      request.input += content;
    }
  }

  std::map<std::string, llm_tool> activated;
  for (const auto& descriptor : activation.exposed_tools) {
    activated.emplace(descriptor.name, descriptor.model_tool());
  }
  if (request.available_tools.empty()) {
    for (auto& [_, tool] : activated) {
      request.available_tools.push_back(std::move(tool));
    }
  }
  else {
    std::vector<llm_tool> filtered;
    for (const auto& tool : request.available_tools) {
      const auto found = activated.find(tool.name);
      if (found != activated.end()) {
        filtered.push_back(found->second);
      }
    }
    request.available_tools = std::move(filtered);
  }

  std::string activated_skills;
  for (const auto& package : activation.packages) {
    if (!activated_skills.empty()) {
      activated_skills.push_back(',');
    }
    activated_skills += package->descriptor().id + "@" + package->descriptor().version.string();
  }
  request.metadata["wuwe.skills"] = std::move(activated_skills);
  request.metadata["wuwe.skills.fingerprint"] = activation.fingerprint;
  return request;
}

} // namespace wuwe::agent::skills

#endif // WUWE_AGENT_SKILLS_PLANNING_ADAPTER_HPP
