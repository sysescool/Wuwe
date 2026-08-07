#ifndef WUWE_AGENT_RUNTIME_RUN_TYPES_HPP
#define WUWE_AGENT_RUNTIME_RUN_TYPES_HPP

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/core/execution_context.hpp>
#include <wuwe/agent/tools/tool_contract.hpp>

namespace wuwe::agent::runtime {

enum class agent_run_status {
  created,
  running,
  waiting_for_approval,
  completed,
  failed,
  cancelled,
  timed_out,
};

enum class approval_resolution {
  pending,
  approved,
  denied,
};

[[nodiscard]] inline std::string to_string(agent_run_status status) {
  switch (status) {
    case agent_run_status::created:
      return "created";
    case agent_run_status::running:
      return "running";
    case agent_run_status::waiting_for_approval:
      return "waiting_for_approval";
    case agent_run_status::completed:
      return "completed";
    case agent_run_status::failed:
      return "failed";
    case agent_run_status::cancelled:
      return "cancelled";
    case agent_run_status::timed_out:
      return "timed_out";
  }
  return "failed";
}

[[nodiscard]] inline agent_run_status agent_run_status_from_string(const std::string& value) {
  if (value == "created")
    return agent_run_status::created;
  if (value == "running")
    return agent_run_status::running;
  if (value == "waiting_for_approval") {
    return agent_run_status::waiting_for_approval;
  }
  if (value == "completed")
    return agent_run_status::completed;
  if (value == "cancelled")
    return agent_run_status::cancelled;
  if (value == "timed_out")
    return agent_run_status::timed_out;
  if (value == "failed")
    return agent_run_status::failed;
  throw std::invalid_argument("invalid agent run status: " + value);
}

[[nodiscard]] inline std::string to_string(approval_resolution resolution) {
  switch (resolution) {
    case approval_resolution::pending:
      return "pending";
    case approval_resolution::approved:
      return "approved";
    case approval_resolution::denied:
      return "denied";
  }
  return "pending";
}

[[nodiscard]] inline bool terminal(agent_run_status status) noexcept {
  return status == agent_run_status::completed || status == agent_run_status::failed ||
         status == agent_run_status::cancelled || status == agent_run_status::timed_out;
}

struct agent_run_suspension {
  std::string approval_id;
  std::string continuation_token;
  std::string tool_call_id;
  std::string tool_name;
  approval_resolution resolution { approval_resolution::pending };
  std::string reason;
  nlohmann::json continuation;
  std::chrono::system_clock::time_point created_at { std::chrono::system_clock::now() };
  std::optional<std::chrono::system_clock::time_point> resolved_at;
  std::map<std::string, std::string> metadata;
};

struct admitted_tool_result {
  std::string tool_call_id;
  std::string idempotency_key;
  std::string tool_name;
  tools::tool_outcome outcome;
  std::chrono::system_clock::time_point admitted_at { std::chrono::system_clock::now() };
};

struct tool_output_projection_audit {
  std::string tool_call_id;
  std::string tool_name;
  std::string projected_content_sha256;
  agent::llm::tool_output_projection_report report;
  std::chrono::system_clock::time_point recorded_at { std::chrono::system_clock::now() };
};

struct agent_run_record {
  std::string id;
  std::uint64_t revision { 0 };
  agent_run_status status { agent_run_status::created };
  core::agent_execution_context context;
  std::chrono::system_clock::time_point created_at { std::chrono::system_clock::now() };
  std::chrono::system_clock::time_point updated_at { created_at };
  std::optional<std::chrono::system_clock::time_point> completed_at;
  std::optional<agent_run_suspension> suspension;
  std::optional<agent_run_suspension> active_continuation;
  std::map<std::string, admitted_tool_result> admitted_tool_results;
  std::map<std::string, tool_output_projection_audit> tool_output_projections;
  nlohmann::json result;
  std::string error;
  std::map<std::string, std::string> metadata;
};

struct agent_run_event {
  std::string run_id;
  std::uint64_t sequence { 0 };
  std::string type;
  agent_run_status status { agent_run_status::created };
  std::string step_id;
  std::string tool_call_id;
  std::chrono::system_clock::time_point timestamp { std::chrono::system_clock::now() };
  nlohmann::json data;
  std::map<std::string, std::string> metadata;
};

namespace detail {

inline std::int64_t to_unix_millis(std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

inline std::chrono::system_clock::time_point from_unix_millis(std::int64_t value) {
  return std::chrono::system_clock::time_point(std::chrono::milliseconds(value));
}

inline agent_run_status run_status_from_string(const std::string& value) {
  return agent_run_status_from_string(value);
}

inline approval_resolution approval_resolution_from_string(const std::string& value) {
  if (value == "approved")
    return approval_resolution::approved;
  if (value == "denied")
    return approval_resolution::denied;
  if (value == "pending")
    return approval_resolution::pending;
  throw std::invalid_argument("invalid approval resolution: " + value);
}

inline tools::tool_error_category tool_error_category_from_string(const std::string& value) {
  using enum tools::tool_error_category;
  if (value == "none")
    return none;
  if (value == "invalid_input")
    return invalid_input;
  if (value == "not_found")
    return not_found;
  if (value == "permission_denied")
    return permission_denied;
  if (value == "conflict")
    return conflict;
  if (value == "rate_limited")
    return rate_limited;
  if (value == "timeout")
    return timeout;
  if (value == "cancelled")
    return cancelled;
  if (value == "unavailable")
    return unavailable;
  if (value == "internal")
    return internal;
  throw std::invalid_argument("invalid tool error category: " + value);
}

inline agent::llm::tool_output_projection_limit tool_output_projection_limit_from_string(
  const std::string& value) {
  using enum agent::llm::tool_output_projection_limit;
  if (value == "none")
    return none;
  if (value == "bytes")
    return bytes;
  if (value == "tokens")
    return tokens;
  if (value == "bytes_and_tokens")
    return bytes_and_tokens;
  throw std::invalid_argument("invalid tool output projection limit: " + value);
}

inline nlohmann::json tool_output_projection_report_to_json(
  const agent::llm::tool_output_projection_report& value) {
  return {
    { "truncated", value.truncated },
    { "original_bytes", value.original_bytes },
    { "projected_bytes", value.projected_bytes },
    { "original_estimated_tokens", value.original_estimated_tokens },
    { "projected_estimated_tokens", value.projected_estimated_tokens },
    { "max_bytes", value.max_bytes },
    { "max_tokens", value.max_tokens },
    { "limiting_factor", agent::llm::to_string(value.limiting_factor) },
  };
}

inline agent::llm::tool_output_projection_report tool_output_projection_report_from_json(
  const nlohmann::json& value) {
  return {
    .truncated = value.value("truncated", false),
    .original_bytes = value.value("original_bytes", std::size_t {}),
    .projected_bytes = value.value("projected_bytes", std::size_t {}),
    .original_estimated_tokens = value.value("original_estimated_tokens", std::size_t {}),
    .projected_estimated_tokens = value.value("projected_estimated_tokens", std::size_t {}),
    .max_bytes = value.value("max_bytes", std::size_t {}),
    .max_tokens = value.value("max_tokens", std::size_t {}),
    .limiting_factor =
      tool_output_projection_limit_from_string(value.value("limiting_factor", std::string("none"))),
  };
}

inline bool canonical_sha256(const std::string& value) noexcept {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
  });
}

inline bool valid_tool_output_projection_report(
  const agent::llm::tool_output_projection_report& value) noexcept {
  if (value.max_bytes < agent::llm::minimum_tool_output_projection_max_bytes ||
      value.max_tokens < agent::llm::minimum_tool_output_projection_max_tokens ||
      value.projected_bytes > value.max_bytes ||
      value.projected_estimated_tokens > value.max_tokens) {
    return false;
  }

  if (!value.truncated) {
    return value.original_bytes == value.projected_bytes &&
           value.original_estimated_tokens == value.projected_estimated_tokens &&
           value.limiting_factor == agent::llm::tool_output_projection_limit::none;
  }

  const bool byte_limited = value.original_bytes > value.max_bytes;
  const bool token_limited = value.original_estimated_tokens > value.max_tokens;
  using enum agent::llm::tool_output_projection_limit;
  const auto expected = byte_limited && token_limited ? bytes_and_tokens
                        : byte_limited                ? bytes
                        : token_limited               ? tokens
                                                      : none;
  return value.limiting_factor == expected && expected != none;
}

inline nlohmann::json tool_outcome_to_json(const tools::tool_outcome& value) {
  return {
    { "content", value.content },
    { "error_value", value.error_code.value() },
    { "error_category_name", value.error_code.category().name() },
    { "error_message", value.error_code.message() },
    { "data", value.data },
    { "category", tools::to_string(value.error_category) },
    { "retryable", value.retryable },
    { "resource_version",
      value.resource_version ? nlohmann::json(*value.resource_version) : nlohmann::json(nullptr) },
    { "compensation_required", value.compensation_required },
    { "compensation_token", value.compensation_token },
    { "artifacts", value.artifacts },
    { "metadata", value.metadata },
  };
}

inline tools::tool_outcome tool_outcome_from_json(const nlohmann::json& value) {
  tools::tool_outcome outcome;
  outcome.content = value.value("content", std::string {});
  const auto error_value = value.value("error_value", 0);
  const auto error_category = value.value("error_category_name", std::string {});
  if (error_value != 0) {
    if (error_category == std::generic_category().name()) {
      outcome.error_code = std::error_code(error_value, std::generic_category());
    }
    else if (error_category == std::system_category().name()) {
      outcome.error_code = std::error_code(error_value, std::system_category());
    }
    else {
      outcome.error_code = std::make_error_code(std::errc::io_error);
      outcome.metadata["persisted_error_value"] = std::to_string(error_value);
      outcome.metadata["persisted_error_category"] = error_category;
      outcome.metadata["persisted_error_message"] = value.value("error_message", std::string {});
    }
  }
  outcome.data = value.value("data", nlohmann::json {});
  outcome.error_category =
    tool_error_category_from_string(value.value("category", std::string("internal")));
  outcome.retryable = value.value("retryable", false);
  if (value.contains("resource_version") && !value.at("resource_version").is_null()) {
    outcome.resource_version = value.at("resource_version").get<std::string>();
  }
  outcome.compensation_required = value.value("compensation_required", false);
  outcome.compensation_token = value.value("compensation_token", std::string {});
  outcome.artifacts = value.value("artifacts", std::vector<std::string> {});
  const auto metadata = value.value("metadata", std::map<std::string, std::string> {});
  outcome.metadata.insert(metadata.begin(), metadata.end());
  return outcome;
}

} // namespace detail

inline nlohmann::json run_suspension_to_json(const agent_run_suspension& value) {
  nlohmann::json output {
    { "approval_id", value.approval_id },
    { "continuation_token", value.continuation_token },
    { "tool_call_id", value.tool_call_id },
    { "tool_name", value.tool_name },
    { "resolution", to_string(value.resolution) },
    { "reason", value.reason },
    { "continuation", value.continuation },
    { "created_at_unix_ms", detail::to_unix_millis(value.created_at) },
    { "metadata", value.metadata },
  };
  output["resolved_at_unix_ms"] = value.resolved_at
                                    ? nlohmann::json(detail::to_unix_millis(*value.resolved_at))
                                    : nlohmann::json(nullptr);
  return output;
}

inline agent_run_suspension run_suspension_from_json(const nlohmann::json& value) {
  agent_run_suspension suspension;
  suspension.approval_id = value.value("approval_id", std::string {});
  suspension.continuation_token = value.value("continuation_token", std::string {});
  suspension.tool_call_id = value.value("tool_call_id", std::string {});
  suspension.tool_name = value.value("tool_name", std::string {});
  suspension.resolution =
    detail::approval_resolution_from_string(value.value("resolution", std::string("pending")));
  suspension.reason = value.value("reason", std::string {});
  suspension.continuation = value.value("continuation", nlohmann::json {});
  suspension.created_at =
    detail::from_unix_millis(value.value("created_at_unix_ms", std::int64_t {}));
  if (value.contains("resolved_at_unix_ms") && !value.at("resolved_at_unix_ms").is_null()) {
    suspension.resolved_at =
      detail::from_unix_millis(value.at("resolved_at_unix_ms").get<std::int64_t>());
  }
  suspension.metadata = value.value("metadata", std::map<std::string, std::string> {});
  if (suspension.approval_id.empty() || suspension.continuation_token.empty() ||
      suspension.tool_call_id.empty() || suspension.tool_name.empty()) {
    throw std::invalid_argument("persisted agent run suspension has incomplete identity fields");
  }
  if (suspension.resolution == approval_resolution::pending && suspension.resolved_at) {
    throw std::invalid_argument("pending agent run suspension cannot have a resolution timestamp");
  }
  if (suspension.resolution != approval_resolution::pending && !suspension.resolved_at) {
    throw std::invalid_argument("resolved agent run suspension requires a resolution timestamp");
  }
  return suspension;
}

inline nlohmann::json agent_run_record_to_json(const agent_run_record& value) {
  nlohmann::json admitted = nlohmann::json::object();
  for (const auto& [call_id, result] : value.admitted_tool_results) {
    admitted[call_id] = {
      { "tool_call_id", result.tool_call_id },
      { "idempotency_key", result.idempotency_key },
      { "tool_name", result.tool_name },
      { "outcome", detail::tool_outcome_to_json(result.outcome) },
      { "admitted_at_unix_ms", detail::to_unix_millis(result.admitted_at) },
    };
  }
  nlohmann::json projections = nlohmann::json::object();
  for (const auto& [call_id, projection] : value.tool_output_projections) {
    projections[call_id] = {
      { "tool_call_id", projection.tool_call_id },
      { "tool_name", projection.tool_name },
      { "projected_content_sha256", projection.projected_content_sha256 },
      { "report", detail::tool_output_projection_report_to_json(projection.report) },
      { "recorded_at_unix_ms", detail::to_unix_millis(projection.recorded_at) },
    };
  }
  nlohmann::json output {
    { "schema_version", 2 },
    { "id", value.id },
    { "revision", value.revision },
    { "status", to_string(value.status) },
    { "context", core::execution_context_to_json(value.context) },
    { "created_at_unix_ms", detail::to_unix_millis(value.created_at) },
    { "updated_at_unix_ms", detail::to_unix_millis(value.updated_at) },
    { "admitted_tool_results", std::move(admitted) },
    { "tool_output_projections", std::move(projections) },
    { "result", value.result },
    { "error", value.error },
    { "metadata", value.metadata },
  };
  output["completed_at_unix_ms"] = value.completed_at
                                     ? nlohmann::json(detail::to_unix_millis(*value.completed_at))
                                     : nlohmann::json(nullptr);
  output["suspension"] =
    value.suspension ? run_suspension_to_json(*value.suspension) : nlohmann::json(nullptr);
  output["active_continuation"] = value.active_continuation
                                    ? run_suspension_to_json(*value.active_continuation)
                                    : nlohmann::json(nullptr);
  return output;
}

inline agent_run_record agent_run_record_from_json(const nlohmann::json& value) {
  const auto schema_version = value.value("schema_version", 0);
  if (schema_version != 1 && schema_version != 2) {
    throw std::invalid_argument("unsupported agent run record schema version");
  }
  agent_run_record record;
  record.id = value.at("id").get<std::string>();
  record.revision = value.value("revision", std::uint64_t {});
  record.status = detail::run_status_from_string(value.value("status", std::string("failed")));
  record.context =
    core::execution_context_from_json(value.value("context", nlohmann::json::object()));
  if (record.context.run_id.empty()) {
    record.context.run_id = record.id;
  }
  else if (record.context.run_id != record.id) {
    throw std::invalid_argument("agent run context id does not match the persisted record id");
  }
  record.created_at = detail::from_unix_millis(value.value("created_at_unix_ms", std::int64_t {}));
  record.updated_at = detail::from_unix_millis(value.value("updated_at_unix_ms", std::int64_t {}));
  if (value.contains("completed_at_unix_ms") && !value.at("completed_at_unix_ms").is_null()) {
    record.completed_at =
      detail::from_unix_millis(value.at("completed_at_unix_ms").get<std::int64_t>());
  }
  if (value.contains("suspension") && !value.at("suspension").is_null()) {
    record.suspension = run_suspension_from_json(value.at("suspension"));
  }
  if (value.contains("active_continuation") && !value.at("active_continuation").is_null()) {
    record.active_continuation = run_suspension_from_json(value.at("active_continuation"));
  }
  if (value.contains("admitted_tool_results") && value.at("admitted_tool_results").is_object()) {
    for (const auto& [call_id, item] : value.at("admitted_tool_results").items()) {
      admitted_tool_result result;
      result.tool_call_id = item.value("tool_call_id", call_id);
      result.idempotency_key = item.value("idempotency_key", std::string {});
      result.tool_name = item.value("tool_name", std::string {});
      result.outcome =
        detail::tool_outcome_from_json(item.value("outcome", nlohmann::json::object()));
      result.admitted_at =
        detail::from_unix_millis(item.value("admitted_at_unix_ms", std::int64_t {}));
      record.admitted_tool_results[call_id] = std::move(result);
    }
  }
  if (schema_version >= 2 && value.contains("tool_output_projections") &&
      value.at("tool_output_projections").is_object()) {
    for (const auto& [call_id, item] : value.at("tool_output_projections").items()) {
      tool_output_projection_audit projection {
        .tool_call_id = item.value("tool_call_id", call_id),
        .tool_name = item.value("tool_name", std::string {}),
        .projected_content_sha256 = item.value("projected_content_sha256", std::string {}),
        .report = detail::tool_output_projection_report_from_json(
          item.value("report", nlohmann::json::object())),
        .recorded_at = detail::from_unix_millis(item.value("recorded_at_unix_ms", std::int64_t {})),
      };
      record.tool_output_projections[call_id] = std::move(projection);
    }
  }
  record.result = value.value("result", nlohmann::json {});
  record.error = value.value("error", std::string {});
  record.metadata = value.value("metadata", std::map<std::string, std::string> {});
  if (record.id.empty()) {
    throw std::invalid_argument("persisted agent run requires an id");
  }
  if (record.status == agent_run_status::waiting_for_approval && !record.suspension) {
    throw std::invalid_argument("waiting agent run requires a suspended approval");
  }
  if (record.status != agent_run_status::waiting_for_approval && record.suspension) {
    throw std::invalid_argument("only a waiting agent run may contain a suspended approval");
  }
  if (record.active_continuation &&
      (record.status != agent_run_status::running ||
        record.active_continuation->resolution != approval_resolution::approved)) {
    throw std::invalid_argument(
      "active continuation requires a running agent run and approved resolution");
  }
  if (terminal(record.status) && !record.completed_at) {
    throw std::invalid_argument("terminal agent run requires a completion timestamp");
  }
  if (!terminal(record.status) && record.completed_at) {
    throw std::invalid_argument("non-terminal agent run cannot contain a completion timestamp");
  }
  for (const auto& [call_id, result] : record.admitted_tool_results) {
    if (call_id.empty() || result.tool_call_id != call_id) {
      throw std::invalid_argument("persisted admitted tool result has an inconsistent call id");
    }
  }
  for (const auto& [call_id, projection] : record.tool_output_projections) {
    if (call_id.empty() || projection.tool_call_id != call_id || projection.tool_name.empty() ||
        !detail::canonical_sha256(projection.projected_content_sha256)) {
      throw std::invalid_argument("persisted tool output projection has invalid audit fields");
    }
    const auto admitted = record.admitted_tool_results.find(call_id);
    if (admitted == record.admitted_tool_results.end() ||
        admitted->second.tool_name != projection.tool_name) {
      throw std::invalid_argument(
        "persisted tool output projection does not match an admitted tool result");
    }
    if (!detail::valid_tool_output_projection_report(projection.report)) {
      throw std::invalid_argument("persisted tool output projection has an invalid report");
    }
  }
  return record;
}

inline nlohmann::json agent_run_event_to_json(const agent_run_event& value) {
  return {
    { "schema_version", 1 },
    { "run_id", value.run_id },
    { "sequence", value.sequence },
    { "type", value.type },
    { "status", to_string(value.status) },
    { "step_id", value.step_id },
    { "tool_call_id", value.tool_call_id },
    { "timestamp_unix_ms", detail::to_unix_millis(value.timestamp) },
    { "data", value.data },
    { "metadata", value.metadata },
  };
}

inline agent_run_event agent_run_event_from_json(const nlohmann::json& value) {
  if (value.value("schema_version", 0) != 1) {
    throw std::invalid_argument("unsupported agent run event schema version");
  }
  agent_run_event event;
  event.run_id = value.at("run_id").get<std::string>();
  event.sequence = value.value("sequence", std::uint64_t {});
  event.type = value.value("type", std::string {});
  event.status = detail::run_status_from_string(value.value("status", std::string("failed")));
  event.step_id = value.value("step_id", std::string {});
  event.tool_call_id = value.value("tool_call_id", std::string {});
  event.timestamp = detail::from_unix_millis(value.value("timestamp_unix_ms", std::int64_t {}));
  event.data = value.value("data", nlohmann::json {});
  event.metadata = value.value("metadata", std::map<std::string, std::string> {});
  if (event.run_id.empty() || event.type.empty() || event.sequence == 0) {
    throw std::invalid_argument("persisted agent run event has incomplete identity fields");
  }
  return event;
}

} // namespace wuwe::agent::runtime

#endif // WUWE_AGENT_RUNTIME_RUN_TYPES_HPP
