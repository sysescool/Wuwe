#ifndef WUWE_AGENT_SKILLS_SKILL_ACTIVATION_HPP
#define WUWE_AGENT_SKILLS_SKILL_ACTIVATION_HPP

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <wuwe/agent/core/execution_context.hpp>
#include <wuwe/agent/core/observability.hpp>
#include <wuwe/agent/skills/skill_resolver.hpp>
#include <wuwe/agent/tools/tool_contract.hpp>

namespace wuwe::agent::skills {

struct skill_activation_limits {
  std::size_t max_packages { 64 };
  std::size_t max_instruction_bytes { 64 * 1024 };
  std::size_t max_tools { 128 };
  std::size_t max_capabilities { 128 };
  std::size_t max_knowledge_requirements { 128 };
};

struct skill_runtime_catalog {
  std::vector<tools::tool_descriptor> tools;
  std::set<std::string> knowledge_sources;
};

struct activated_skill_instruction {
  std::string skill_id;
  std::string resource_id;
  std::string content;
  core::content_provenance provenance;
};

struct skill_activation_request {
  skill_resolution_result resolution;
  skill_runtime_catalog catalog;
  skill_activation_limits limits;
  core::agent_execution_context context;
};

struct skill_activation_result {
  bool success { false };
  std::vector<skill_package_ptr> packages;
  std::vector<activated_skill_instruction> instructions;
  std::vector<tools::tool_descriptor> exposed_tools;
  std::vector<skill_capability_requirement> declared_capabilities;
  std::vector<skill_knowledge_requirement> knowledge;
  std::vector<skill_diagnostic> diagnostics;
  std::string fingerprint;

  [[nodiscard]] explicit operator bool() const noexcept {
    return success;
  }

  [[nodiscard]] std::vector<std::string> tool_names() const;
};

struct skill_activator_options {
  observability::event_sink* event_sink {};
  observability::telemetry_failure_mode telemetry_failure {
    observability::telemetry_failure_mode::ignore,
  };
};

class skill_activator final {
public:
  explicit skill_activator(skill_activator_options options = {}) : options_(options) {
  }

  [[nodiscard]] skill_activation_result activate(const skill_activation_request& request) const;

private:
  skill_activator_options options_;
};

} // namespace wuwe::agent::skills

#endif // WUWE_AGENT_SKILLS_SKILL_ACTIVATION_HPP
