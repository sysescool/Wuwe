#include <wuwe/agent/skills/skills.hpp>

#ifdef WUWE_AGENT_A2A_SKILLS_ADAPTER_HPP
#error "skills.hpp must not force the optional A2A adapter"
#endif

#ifdef WUWE_AGENT_MCP_SKILLS_ADAPTER_HPP
#error "skills.hpp must not force the optional MCP adapter"
#endif

#ifdef WUWE_AGENT_MULTI_AGENT_SKILLS_ADAPTER_HPP
#error "skills.hpp must not force the optional Multi-Agent adapter"
#endif

bool skills_header_is_independent() {
  const auto version = wuwe::agent::skills::semantic_version::parse("1.0.0");
  return version.major == 1 && wuwe::agent::skills::current_skill_manifest_schema_version == 1;
}
