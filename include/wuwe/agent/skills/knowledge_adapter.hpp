#ifndef WUWE_AGENT_SKILLS_KNOWLEDGE_ADAPTER_HPP
#define WUWE_AGENT_SKILLS_KNOWLEDGE_ADAPTER_HPP

#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/core/content.hpp>
#include <wuwe/agent/knowledge/knowledge_record.hpp>
#include <wuwe/agent/skills/skill_activation.hpp>

namespace wuwe::agent::skills {

[[nodiscard]] inline std::vector<knowledge::knowledge_document> knowledge_documents_from_activation(
  const skill_activation_result& activation) {
  std::vector<knowledge::knowledge_document> output;
  if (!activation) {
    return output;
  }
  for (const auto& package : activation.packages) {
    const auto& descriptor = package->descriptor();
    for (const auto& [resource_id, resource] : package->resources()) {
      if (resource.descriptor.kind != skill_resource_kind::knowledge) {
        continue;
      }
      knowledge::knowledge_document document {
        .id = descriptor.id + "@" + descriptor.version.string() + ":" + resource_id,
        .title = descriptor.name + " / " + resource_id,
        .content = resource.content,
        .source_uri = package->provenance().source_uri,
        .metadata = {
          { "wuwe.skill.id", descriptor.id },
          { "wuwe.skill.version", descriptor.version.string() },
          { "wuwe.skill.package_sha256", package->provenance().content_sha256 },
          { "wuwe.skill.resource_sha256", resource.sha256 },
        },
      };
      core::set_content_provenance(document.metadata,
        {
          .trust = core::least_trusted(
            package->provenance().trust, core::content_trust_level::retrieved_untrusted),
          .source = core::content_source_kind::knowledge,
          .source_id = document.id,
          .source_uri = document.source_uri,
        });
      output.push_back(std::move(document));
    }
  }
  return output;
}

} // namespace wuwe::agent::skills

#endif // WUWE_AGENT_SKILLS_KNOWLEDGE_ADAPTER_HPP
