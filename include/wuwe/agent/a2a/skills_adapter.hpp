#ifndef WUWE_AGENT_A2A_SKILLS_ADAPTER_HPP
#define WUWE_AGENT_A2A_SKILLS_ADAPTER_HPP

#include <stdexcept>
#include <string>
#include <utility>

#include <wuwe/agent/a2a/a2a_types.hpp>
#include <wuwe/agent/skills/skill_package.hpp>

namespace wuwe::agent::a2a {

struct skill_publication_options {
  bool publish_version { true };
  bool publish_digest { true };
  bool publish_schema_version { true };
};

struct external_skill_reference {
  skills::skill_descriptor descriptor;
  std::string agent_url;
  bool activatable { false };
  core::content_trust_level trust { core::content_trust_level::retrieved_untrusted };
};

[[nodiscard]] inline agent_skill agent_skill_from_descriptor(
  const skills::skill_descriptor& descriptor) {
  return {
    .id = descriptor.id,
    .name = descriptor.name,
    .description = descriptor.description,
    .tags = descriptor.tags,
    .examples = descriptor.examples,
    .input_modes = descriptor.input_modes,
    .output_modes = descriptor.output_modes,
  };
}

inline void publish_skill(
  agent_card& card, const skills::skill_package& package, skill_publication_options options = {}) {
  const auto& descriptor = package.descriptor();
  for (const auto& existing : card.skills) {
    if (existing.id == descriptor.id) {
      throw std::invalid_argument("duplicate A2A skill publication: " + descriptor.id);
    }
  }
  card.skills.push_back(agent_skill_from_descriptor(descriptor));

  auto& extension = card.metadata["wuwe"]["skills"][descriptor.id];
  extension = nlohmann::json::object();
  if (options.publish_version) {
    extension["version"] = descriptor.version.string();
  }
  if (options.publish_digest) {
    extension["digest"] = package.provenance().content_sha256;
  }
  if (options.publish_schema_version) {
    extension["manifest_schema_version"] = package.manifest().schema_version;
  }
}

[[nodiscard]] inline external_skill_reference external_skill_reference_from_card(
  const agent_card& card, const agent_skill& advertised) {
  skills::semantic_version version;
  try {
    const auto& skill_metadata = card.metadata.at("wuwe").at("skills").at(advertised.id);
    if (const auto found = skill_metadata.find("version");
        found != skill_metadata.end() && found->is_string()) {
      version = skills::semantic_version::parse(found->get_ref<const std::string&>());
    }
  }
  catch (...) {
    version = {};
  }
  return {
    .descriptor = {
      .id = advertised.id,
      .name = advertised.name,
      .description = advertised.description,
      .version = std::move(version),
      .tags = advertised.tags,
      .examples = advertised.examples,
      .input_modes = advertised.input_modes,
      .output_modes = advertised.output_modes,
      .metadata = { { "wuwe.skill.remote_advertisement", "true" } },
    },
    .agent_url = card.url,
    .activatable = false,
    .trust = core::content_trust_level::retrieved_untrusted,
  };
}

} // namespace wuwe::agent::a2a

#endif // WUWE_AGENT_A2A_SKILLS_ADAPTER_HPP
