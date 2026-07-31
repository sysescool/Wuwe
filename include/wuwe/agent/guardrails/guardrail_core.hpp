#ifndef WUWE_AGENT_GUARDRAILS_GUARDRAIL_CORE_HPP
#define WUWE_AGENT_GUARDRAILS_GUARDRAIL_CORE_HPP

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace wuwe::agent::guardrails {

enum class guardrail_stage {
  input,
  output,
  tool_input,
  tool_output,
  planning,
  retrieval,
  memory_write,
};

enum class guardrail_decision {
  allow,
  modify,
  deny,
  require_approval,
};

enum class guardrail_severity {
  info,
  warning,
  error,
  critical,
};

[[nodiscard]] inline std::string to_string(guardrail_stage stage) {
  switch (stage) {
    case guardrail_stage::input:
      return "input";
    case guardrail_stage::output:
      return "output";
    case guardrail_stage::tool_input:
      return "tool_input";
    case guardrail_stage::tool_output:
      return "tool_output";
    case guardrail_stage::planning:
      return "planning";
    case guardrail_stage::retrieval:
      return "retrieval";
    case guardrail_stage::memory_write:
      return "memory_write";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(guardrail_decision decision) {
  switch (decision) {
    case guardrail_decision::allow:
      return "allow";
    case guardrail_decision::modify:
      return "modify";
    case guardrail_decision::deny:
      return "deny";
    case guardrail_decision::require_approval:
      return "require_approval";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(guardrail_severity severity) {
  switch (severity) {
    case guardrail_severity::info:
      return "info";
    case guardrail_severity::warning:
      return "warning";
    case guardrail_severity::error:
      return "error";
    case guardrail_severity::critical:
      return "critical";
  }
  return "unknown";
}

struct guardrail_request {
  guardrail_stage stage { guardrail_stage::input };
  std::string subject_id;
  std::string content;
  nlohmann::json data;
  std::map<std::string, std::string> metadata;
};

struct guardrail_issue {
  guardrail_severity severity { guardrail_severity::warning };
  std::string code;
  std::string message;
  std::string evidence;
  std::string remediation;
  std::map<std::string, std::string> metadata;
};

struct guardrail_result {
  guardrail_decision decision { guardrail_decision::allow };
  std::optional<std::string> replacement_content;
  std::optional<nlohmann::json> replacement_data;
  std::vector<guardrail_issue> issues;
  std::map<std::string, std::string> metadata;

  [[nodiscard]] static guardrail_result allow() {
    return {};
  }

  [[nodiscard]] static guardrail_result modify(std::string content, guardrail_issue issue = {}) {
    guardrail_result result;
    result.decision = guardrail_decision::modify;
    result.replacement_content = std::move(content);
    if (!issue.code.empty() || !issue.message.empty()) {
      result.issues.push_back(std::move(issue));
    }
    return result;
  }

  [[nodiscard]] static guardrail_result modify_data(
    nlohmann::json data, guardrail_issue issue = {}) {
    guardrail_result result;
    result.decision = guardrail_decision::modify;
    result.replacement_data = std::move(data);
    if (!issue.code.empty() || !issue.message.empty()) {
      result.issues.push_back(std::move(issue));
    }
    return result;
  }

  [[nodiscard]] static guardrail_result deny(guardrail_issue issue) {
    guardrail_result result;
    result.decision = guardrail_decision::deny;
    result.issues.push_back(std::move(issue));
    return result;
  }

  [[nodiscard]] static guardrail_result require_approval(guardrail_issue issue) {
    guardrail_result result;
    result.decision = guardrail_decision::require_approval;
    result.issues.push_back(std::move(issue));
    return result;
  }
};

struct guardrail_check_result {
  std::string guardrail_name;
  guardrail_result result;
  std::chrono::milliseconds elapsed { 0 };
  std::string error;
};

struct guardrail_run_result {
  guardrail_stage stage { guardrail_stage::input };
  std::string subject_id;
  guardrail_decision decision { guardrail_decision::allow };
  std::string content;
  nlohmann::json data;
  std::vector<guardrail_issue> issues;
  std::vector<guardrail_check_result> checks;
  std::map<std::string, std::string> metadata;
  std::chrono::milliseconds elapsed { 0 };

  [[nodiscard]] bool allowed() const noexcept {
    return decision == guardrail_decision::allow || decision == guardrail_decision::modify;
  }

  [[nodiscard]] bool modified() const noexcept {
    return decision == guardrail_decision::modify;
  }
};

class guardrail {
public:
  virtual ~guardrail() = default;

  [[nodiscard]] virtual std::string name() const = 0;
  virtual guardrail_result evaluate(const guardrail_request& request) const = 0;
};

class function_guardrail final : public guardrail {
public:
  using callback = std::function<guardrail_result(const guardrail_request&)>;

  function_guardrail(std::string name, callback evaluate)
      : name_(std::move(name)), evaluate_(std::move(evaluate)) {
    if (name_.empty()) {
      throw std::invalid_argument("function_guardrail requires a name");
    }
    if (!evaluate_) {
      throw std::invalid_argument("function_guardrail requires a callback");
    }
  }

  [[nodiscard]] std::string name() const override {
    return name_;
  }

  guardrail_result evaluate(const guardrail_request& request) const override {
    return evaluate_(request);
  }

private:
  std::string name_;
  callback evaluate_;
};

using guardrail_observer = std::function<void(const guardrail_run_result&)>;

inline nlohmann::json guardrail_issue_to_json(const guardrail_issue& issue) {
  return {
    { "severity", to_string(issue.severity) },
    { "code", issue.code },
    { "message", issue.message },
    { "evidence", issue.evidence },
    { "remediation", issue.remediation },
    { "metadata", issue.metadata },
  };
}

inline nlohmann::json guardrail_run_result_to_json(const guardrail_run_result& result) {
  auto issues = nlohmann::json::array();
  for (const auto& issue : result.issues) {
    issues.push_back(guardrail_issue_to_json(issue));
  }
  auto checks = nlohmann::json::array();
  for (const auto& check : result.checks) {
    checks.push_back({
      { "guardrail", check.guardrail_name },
      { "decision", to_string(check.result.decision) },
      { "elapsed_ms", check.elapsed.count() },
      { "error", check.error },
    });
  }
  return {
    { "stage", to_string(result.stage) },
    { "subject_id", result.subject_id },
    { "decision", to_string(result.decision) },
    { "issues", std::move(issues) },
    { "checks", std::move(checks) },
    { "metadata", result.metadata },
    { "elapsed_ms", result.elapsed.count() },
  };
}

} // namespace wuwe::agent::guardrails

#endif // WUWE_AGENT_GUARDRAILS_GUARDRAIL_CORE_HPP
