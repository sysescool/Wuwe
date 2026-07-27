#ifndef WUWE_AGENT_GUARDRAILS_CONTENT_TRUST_GUARDRAIL_HPP
#define WUWE_AGENT_GUARDRAILS_CONTENT_TRUST_GUARDRAIL_HPP

#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include <wuwe/agent/core/content.hpp>
#include <wuwe/agent/guardrails/guardrail_core.hpp>

namespace wuwe::agent::guardrails {

struct content_trust_guardrail_options {
  std::string name { "content_trust" };
  std::set<guardrail_stage> stages {
    guardrail_stage::input,
    guardrail_stage::tool_output,
    guardrail_stage::retrieval,
    guardrail_stage::memory_write,
  };
  bool deny_untrusted_system_messages { true };
  bool require_explicit_trust_metadata { true };
};

class content_trust_guardrail final : public guardrail {
public:
  explicit content_trust_guardrail(
    content_trust_guardrail_options options = {})
      : options_(std::move(options)) {
    if (options_.name.empty()) {
      throw std::invalid_argument("content_trust_guardrail requires a name");
    }
  }

  [[nodiscard]] std::string name() const override {
    return options_.name;
  }

  guardrail_result evaluate(const guardrail_request& request) const override {
    if (!options_.stages.contains(request.stage)) {
      return guardrail_result::allow();
    }
    const auto trust_value = request.metadata.find("wuwe.content.trust");
    if (trust_value == request.metadata.end()) {
      if (!options_.require_explicit_trust_metadata) {
        return guardrail_result::allow();
      }
      return guardrail_result::deny({
        .severity = guardrail_severity::error,
        .code = "content_trust_missing",
        .message = "content is missing provenance trust metadata",
        .remediation = "label content provenance before it enters the agent context",
      });
    }
    const auto parsed_trust = core::try_content_trust_from_string(trust_value->second);
    if (!parsed_trust) {
      return guardrail_result::deny({
        .severity = guardrail_severity::error,
        .code = "content_trust_invalid",
        .message = "content has an invalid provenance trust label",
        .remediation = "use a content_trust_level value defined by the framework",
      });
    }
    const auto trust = *parsed_trust;
    const auto role = request.metadata.find("message_role");
    if (options_.deny_untrusted_system_messages &&
        role != request.metadata.end() && role->second == "system" &&
        !core::trusted_for_system_message(trust)) {
      return guardrail_result::deny({
        .severity = guardrail_severity::critical,
        .code = "untrusted_system_content",
        .message = "untrusted content cannot be promoted to a system message",
        .remediation = "render the content inside an untrusted context boundary",
        .metadata = { { "trust", core::to_string(trust) } },
      });
    }
    auto result = guardrail_result::allow();
    result.metadata["trust"] = core::to_string(trust);
    return result;
  }

private:
  content_trust_guardrail_options options_;
};

} // namespace wuwe::agent::guardrails

#endif // WUWE_AGENT_GUARDRAILS_CONTENT_TRUST_GUARDRAIL_HPP
