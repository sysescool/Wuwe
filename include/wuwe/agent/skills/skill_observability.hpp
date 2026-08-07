#ifndef WUWE_AGENT_SKILLS_SKILL_OBSERVABILITY_HPP
#define WUWE_AGENT_SKILLS_SKILL_OBSERVABILITY_HPP

#include <map>
#include <string>
#include <utility>

#include <wuwe/agent/core/observability.hpp>

namespace wuwe::agent::skills {

struct skill_observability_context {
  std::string trace_id;
  std::string subject_id;
  std::string run_id;
  std::string request_id;
};

// Skill telemetry deliberately accepts attributes only. Resource contents,
// instructions, prompts, schemas, and manifest JSON must never be attached to
// observability events by the loader.
inline void publish_skill_event(observability::event_sink* sink,
  const skill_observability_context& context, std::string name,
  std::map<std::string, std::string> attributes = {}) noexcept {
  if (!sink) {
    return;
  }
  try {
    sink->publish({
      .module = "skills",
      .name = std::move(name),
      .trace_id = context.trace_id,
      .subject_id = context.subject_id,
      .run_id = context.run_id,
      .request_id = context.request_id,
      .attributes = std::move(attributes),
    });
  }
  catch (...) {
    // Telemetry is best-effort and must not change skill loading semantics.
  }
}

} // namespace wuwe::agent::skills

#endif // WUWE_AGENT_SKILLS_SKILL_OBSERVABILITY_HPP
