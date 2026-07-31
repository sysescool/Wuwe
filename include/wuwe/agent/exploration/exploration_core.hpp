#ifndef WUWE_AGENT_EXPLORATION_EXPLORATION_CORE_HPP
#define WUWE_AGENT_EXPLORATION_EXPLORATION_CORE_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/approval/approval.hpp>
#include <wuwe/agent/capability/capability.hpp>

namespace wuwe::agent::exploration {

enum class experiment_safety {
  read_only,
  effectful,
};

enum class experiment_status {
  proposed,
  blocked,
  approval_required,
  approved,
  completed,
  failed,
  timed_out,
  cancelled,
};

enum class hypothesis_verdict {
  supported,
  refuted,
  inconclusive,
  blocked,
  failed,
};

enum class exploration_stop_reason {
  none,
  cancelled,
  timed_out,
  generation_failed,
  design_failed,
  review_failed,
};

[[nodiscard]] inline std::string to_string(experiment_safety value) {
  switch (value) {
    case experiment_safety::read_only:
      return "read_only";
    case experiment_safety::effectful:
      return "effectful";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(experiment_status value) {
  switch (value) {
    case experiment_status::proposed:
      return "proposed";
    case experiment_status::blocked:
      return "blocked";
    case experiment_status::approval_required:
      return "approval_required";
    case experiment_status::approved:
      return "approved";
    case experiment_status::completed:
      return "completed";
    case experiment_status::failed:
      return "failed";
    case experiment_status::timed_out:
      return "timed_out";
    case experiment_status::cancelled:
      return "cancelled";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(hypothesis_verdict value) {
  switch (value) {
    case hypothesis_verdict::supported:
      return "supported";
    case hypothesis_verdict::refuted:
      return "refuted";
    case hypothesis_verdict::inconclusive:
      return "inconclusive";
    case hypothesis_verdict::blocked:
      return "blocked";
    case hypothesis_verdict::failed:
      return "failed";
  }
  return "unknown";
}

[[nodiscard]] inline std::string to_string(exploration_stop_reason value) {
  switch (value) {
    case exploration_stop_reason::none:
      return "none";
    case exploration_stop_reason::cancelled:
      return "cancelled";
    case exploration_stop_reason::timed_out:
      return "timed_out";
    case exploration_stop_reason::generation_failed:
      return "generation_failed";
    case exploration_stop_reason::design_failed:
      return "design_failed";
    case exploration_stop_reason::review_failed:
      return "review_failed";
  }
  return "unknown";
}

struct exploration_context {
  std::stop_token stop_token;
  std::optional<std::chrono::steady_clock::time_point> deadline;

  [[nodiscard]] bool cancellation_requested() const noexcept {
    return stop_token.stop_requested();
  }

  [[nodiscard]] bool deadline_reached() const noexcept {
    return deadline && std::chrono::steady_clock::now() >= *deadline;
  }

  [[nodiscard]] std::chrono::milliseconds remaining_time() const noexcept {
    if (!deadline)
      return std::chrono::milliseconds::max();
    const auto now = std::chrono::steady_clock::now();
    if (now >= *deadline)
      return std::chrono::milliseconds { 0 };
    return std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
  }
};

struct exploration_request {
  std::string id;
  std::string objective;
  nlohmann::json context = nlohmann::json::object();
  std::map<std::string, std::string> metadata;
};

struct hypothesis {
  std::string id;
  std::string statement;
  std::string rationale;
  double prior_confidence { 0.5 };
  std::map<std::string, std::string> metadata;
};

struct experiment {
  std::string id;
  std::string hypothesis_id;
  std::string title;
  std::string description;
  experiment_safety safety { experiment_safety::read_only };
  std::vector<capability::capability_request> capabilities;
  nlohmann::json parameters = nlohmann::json::object();
  std::map<std::string, std::string> metadata;
};

struct experiment_evidence {
  bool succeeded { false };
  nlohmann::json observation;
  std::vector<std::string> artifacts;
  std::string summary;
  std::string error;
  std::chrono::milliseconds elapsed { 0 };
  std::map<std::string, std::string> metadata;
};

struct experiment_record {
  experiment specification;
  experiment_status status { experiment_status::proposed };
  std::optional<approval::approval_decision> approval;
  std::optional<experiment_evidence> evidence;
  std::string error;
  bool detached { false };
};

struct hypothesis_assessment {
  hypothesis_verdict verdict { hypothesis_verdict::inconclusive };
  double confidence { 0.0 };
  std::string conclusion;
  std::vector<std::string> supporting_evidence;
  std::vector<std::string> counter_evidence;
  std::map<std::string, std::string> metadata;
};

struct hypothesis_record {
  hypothesis value;
  std::vector<experiment_record> experiments;
  std::optional<hypothesis_assessment> assessment;
  hypothesis_verdict verdict { hypothesis_verdict::inconclusive };
  std::string error;
};

struct exploration_record {
  std::string id;
  exploration_request request;
  std::vector<hypothesis_record> hypotheses;
  exploration_stop_reason stop_reason { exploration_stop_reason::none };
  std::string error;
  std::chrono::system_clock::time_point created_at { std::chrono::system_clock::now() };
  std::chrono::system_clock::time_point updated_at { std::chrono::system_clock::now() };
  std::chrono::milliseconds elapsed { 0 };
  std::size_t detached_count { 0 };
};

struct exploration_run_result {
  bool completed { false };
  exploration_record record;
  std::size_t hypothesis_count { 0 };
  std::size_t experiment_count { 0 };
  std::size_t completed_experiment_count { 0 };
  std::size_t blocked_experiment_count { 0 };
  std::size_t failed_experiment_count { 0 };
  std::size_t supported_count { 0 };
  std::size_t refuted_count { 0 };
  std::size_t inconclusive_count { 0 };
  std::size_t blocked_hypothesis_count { 0 };
  std::size_t failed_hypothesis_count { 0 };
  std::size_t telemetry_error_count { 0 };

  [[nodiscard]] explicit operator bool() const noexcept {
    return completed && record.stop_reason == exploration_stop_reason::none;
  }
};

inline std::string make_exploration_id(const char* prefix) {
  static std::atomic<std::uint64_t> next { 1 };
  const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::system_clock::now().time_since_epoch())
                     .count();
  return std::string(prefix) + "-" + std::to_string(now) + "-" +
         std::to_string(next.fetch_add(1, std::memory_order_relaxed));
}

inline nlohmann::json experiment_record_to_json(const experiment_record& value) {
  auto capabilities = nlohmann::json::array();
  for (const auto& capability : value.specification.capabilities) {
    capabilities.push_back({
      { "name", capability.name },
      { "risk", capability::to_string(capability.risk) },
      { "summary", capability.summary },
      { "resources", capability.resources },
    });
  }
  return {
    { "id", value.specification.id },
    { "hypothesis_id", value.specification.hypothesis_id },
    { "title", value.specification.title },
    { "description", value.specification.description },
    { "safety", to_string(value.specification.safety) },
    { "capabilities", std::move(capabilities) },
    { "parameters", value.specification.parameters },
    { "status", to_string(value.status) },
    { "approval", value.approval ? nlohmann::json {
        { "kind", approval::to_string(value.approval->kind) },
        { "scope", approval::to_string(value.approval->scope) },
        { "reason", value.approval->reason },
      } : nlohmann::json(nullptr) },
    { "evidence", value.evidence ? nlohmann::json {
        { "succeeded", value.evidence->succeeded },
        { "observation", value.evidence->observation },
        { "artifacts", value.evidence->artifacts },
        { "summary", value.evidence->summary },
        { "error", value.evidence->error },
        { "elapsed_ms", value.evidence->elapsed.count() },
      } : nlohmann::json(nullptr) },
    { "error", value.error },
    { "detached", value.detached },
  };
}

inline nlohmann::json exploration_record_to_json(const exploration_record& value) {
  auto hypotheses = nlohmann::json::array();
  for (const auto& item : value.hypotheses) {
    auto experiments = nlohmann::json::array();
    for (const auto& experiment : item.experiments) {
      experiments.push_back(experiment_record_to_json(experiment));
    }
    hypotheses.push_back({
      { "id", item.value.id },
      { "statement", item.value.statement },
      { "rationale", item.value.rationale },
      { "prior_confidence", item.value.prior_confidence },
      { "verdict", to_string(item.verdict) },
      { "assessment", item.assessment ? nlohmann::json {
          { "verdict", to_string(item.assessment->verdict) },
          { "confidence", item.assessment->confidence },
          { "conclusion", item.assessment->conclusion },
          { "supporting_evidence", item.assessment->supporting_evidence },
          { "counter_evidence", item.assessment->counter_evidence },
        } : nlohmann::json(nullptr) },
      { "experiments", std::move(experiments) },
      { "error", item.error },
    });
  }
  return {
    { "id", value.id },
    { "objective", value.request.objective },
    { "stop_reason", to_string(value.stop_reason) },
    { "error", value.error },
    { "elapsed_ms", value.elapsed.count() },
    { "detached_count", value.detached_count },
    { "hypotheses", std::move(hypotheses) },
  };
}

inline nlohmann::json exploration_run_result_to_json(const exploration_run_result& value) {
  return {
    { "completed", value.completed },
    { "hypothesis_count", value.hypothesis_count },
    { "experiment_count", value.experiment_count },
    { "completed_experiment_count", value.completed_experiment_count },
    { "blocked_experiment_count", value.blocked_experiment_count },
    { "failed_experiment_count", value.failed_experiment_count },
    { "supported_count", value.supported_count },
    { "refuted_count", value.refuted_count },
    { "inconclusive_count", value.inconclusive_count },
    { "blocked_hypothesis_count", value.blocked_hypothesis_count },
    { "failed_hypothesis_count", value.failed_hypothesis_count },
    { "telemetry_error_count", value.telemetry_error_count },
    { "record", exploration_record_to_json(value.record) },
  };
}

} // namespace wuwe::agent::exploration

#endif // WUWE_AGENT_EXPLORATION_EXPLORATION_CORE_HPP
