#ifndef WUWE_AGENT_SKILLS_LLM_ADAPTER_HPP
#define WUWE_AGENT_SKILLS_LLM_ADAPTER_HPP

#include <cstddef>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/core/content.hpp>
#include <wuwe/agent/llm/llm_types.h>
#include <wuwe/agent/skills/skill_activation.hpp>

namespace wuwe::agent::skills {

struct skill_llm_activation_policy {
  bool trusted_instructions_as_system_messages { true };
  bool expose_activated_tools { true };
};

[[nodiscard]] inline llm_request apply_skill_activation(llm_request request,
  const skill_activation_result& activation, skill_llm_activation_policy policy = {}) {
  if (!activation) {
    throw std::invalid_argument("cannot apply an unsuccessful skill activation");
  }

  std::vector<chat_message> system_messages;
  std::vector<chat_message> untrusted_messages;
  for (const auto& instruction : activation.instructions) {
    const bool trusted_system = policy.trusted_instructions_as_system_messages &&
                                core::trusted_for_system_message(instruction.provenance.trust);
    auto content = instruction.content;
    if (!trusted_system) {
      content = core::render_context_boundary(
        "skill:" + instruction.skill_id, instruction.provenance.trust, std::move(content));
    }
    chat_message message {
      .role = trusted_system ? "system" : "user",
      .content = std::move(content),
      .context_source = llm_context_source::skill,
    };
    if (trusted_system) {
      system_messages.push_back(std::move(message));
    }
    else {
      untrusted_messages.push_back(std::move(message));
    }
  }

  auto insert_at = request.messages.begin();
  while (insert_at != request.messages.end() && insert_at->role == "system") {
    ++insert_at;
  }
  insert_at = request.messages.insert(insert_at, system_messages.begin(), system_messages.end());
  std::advance(insert_at, static_cast<std::ptrdiff_t>(system_messages.size()));
  request.messages.insert(insert_at, untrusted_messages.begin(), untrusted_messages.end());

  if (policy.expose_activated_tools) {
    std::map<std::string, llm_tool> activated;
    for (const auto& descriptor : activation.exposed_tools) {
      activated.emplace(descriptor.name, descriptor.model_tool());
    }
    if (request.tools.empty()) {
      for (auto& [_, tool] : activated) {
        request.tools.push_back(std::move(tool));
      }
    }
    else {
      std::vector<llm_tool> filtered;
      for (const auto& existing : request.tools) {
        const auto found = activated.find(existing.name);
        if (found != activated.end()) {
          filtered.push_back(found->second);
        }
      }
      request.tools = std::move(filtered);
    }
  }

  if (request.execution_context) {
    std::string activated_skills;
    for (const auto& package : activation.packages) {
      if (!activated_skills.empty()) {
        activated_skills.push_back(',');
      }
      activated_skills += package->descriptor().id + "@" + package->descriptor().version.string();
    }
    request.execution_context->metadata["wuwe.skills"] = std::move(activated_skills);
    request.execution_context->metadata["wuwe.skills.fingerprint"] = activation.fingerprint;
  }
  return request;
}

} // namespace wuwe::agent::skills

#endif // WUWE_AGENT_SKILLS_LLM_ADAPTER_HPP
