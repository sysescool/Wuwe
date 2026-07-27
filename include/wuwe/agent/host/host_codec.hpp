#ifndef WUWE_AGENT_HOST_HOST_CODEC_HPP
#define WUWE_AGENT_HOST_HOST_CODEC_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/host/host_service.hpp>

namespace wuwe::agent::host {

namespace codec_detail {

inline std::int64_t to_unix_millis(
  std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    value.time_since_epoch()).count();
}

inline std::chrono::system_clock::time_point from_unix_millis(
  std::int64_t value) {
  return std::chrono::system_clock::time_point(
    std::chrono::milliseconds(value));
}

inline std::map<std::string, std::string> safe_metadata(
  const std::map<std::string, std::string>& metadata) {
  std::map<std::string, std::string> output;
  for (const auto& [key, value] : metadata) {
    if (!core::sensitive_metadata_key(key)) {
      output.emplace(key, value);
    }
  }
  return output;
}

inline std::string required_string(
  const nlohmann::json& body,
  const char* name) {
  const auto found = body.find(name);
  if (found == body.end() || !found->is_string() ||
      found->get_ref<const std::string&>().empty()) {
    throw std::invalid_argument(
      std::string("agent host request requires non-empty '") + name + "'");
  }
  return found->get<std::string>();
}

inline std::uint64_t required_revision(const nlohmann::json& body) {
  const auto found = body.find("expectedRevision");
  if (found == body.end() || !found->is_number_unsigned()) {
    throw std::invalid_argument(
      "agent host request requires unsigned 'expectedRevision'");
  }
  const auto revision = found->get<std::uint64_t>();
  if (revision == 0) {
    throw std::invalid_argument("agent host expectedRevision must be positive");
  }
  return revision;
}

inline runtime::approval_resolution approval_resolution_from_string(
  const std::string& value) {
  if (value == "approved") return runtime::approval_resolution::approved;
  if (value == "denied") return runtime::approval_resolution::denied;
  throw std::invalid_argument(
    "agent host approval resolution must be 'approved' or 'denied'");
}

inline runtime::approval_resolution approval_state_from_string(
  const std::string& value) {
  if (value == "pending") return runtime::approval_resolution::pending;
  return approval_resolution_from_string(value);
}

} // namespace codec_detail

[[nodiscard]] inline nlohmann::json execution_context_to_host_json(
  const core::agent_execution_context& context) {
  nlohmann::json output {
    { "runId", context.run_id },
    { "traceId", context.trace_id },
    { "requestId", context.request_id },
    { "tenantId", context.tenant_id },
    { "userId", context.user_id },
    { "applicationId", context.application_id },
    { "workspaceId", context.workspace_id },
    { "conversationId", context.conversation_id },
    { "agentId", context.agent_id },
    { "locale", context.locale },
    { "metadata", codec_detail::safe_metadata(context.metadata) },
  };
  output["deadlineUnixMs"] = context.deadline
    ? nlohmann::json(codec_detail::to_unix_millis(*context.deadline))
    : nlohmann::json(nullptr);
  return output;
}

[[nodiscard]] inline core::agent_execution_context
execution_context_from_host_json(const nlohmann::json& value) {
  if (!value.is_object()) {
    throw std::invalid_argument("agent host execution context must be an object");
  }
  core::agent_execution_context context {
    .run_id = value.value("runId", std::string {}),
    .trace_id = value.value("traceId", std::string {}),
    .request_id = value.value("requestId", std::string {}),
    .tenant_id = value.value("tenantId", std::string {}),
    .user_id = value.value("userId", std::string {}),
    .application_id = value.value("applicationId", std::string {}),
    .workspace_id = value.value("workspaceId", std::string {}),
    .conversation_id = value.value("conversationId", std::string {}),
    .agent_id = value.value("agentId", std::string {}),
    .locale = value.value("locale", std::string {}),
    .metadata = value.value(
      "metadata", std::map<std::string, std::string> {}),
  };
  for (const auto& [key, _] : context.metadata) {
    if (core::sensitive_execution_context_metadata_key(key)) {
      throw std::invalid_argument(
        "agent host execution context must not contain sensitive metadata: " +
        key);
    }
  }
  if (value.contains("deadlineUnixMs") &&
      !value.at("deadlineUnixMs").is_null()) {
    context.deadline = codec_detail::from_unix_millis(
      value.at("deadlineUnixMs").get<std::int64_t>());
  }
  return context;
}

[[nodiscard]] inline nlohmann::json create_run_request_to_json(
  const create_run_request& request) {
  if (request.context.run_id.empty()) {
    throw std::invalid_argument(
      "host create_run requires a caller-assigned context.run_id");
  }
  return {
    { "context", execution_context_to_host_json(request.context) },
    { "input", request.input },
    { "metadata", request.metadata },
  };
}

[[nodiscard]] inline create_run_request create_run_request_from_json(
  const nlohmann::json& body) {
  const auto context = body.find("context");
  if (context == body.end() || !context->is_object()) {
    throw std::invalid_argument("create_run requires an object 'context'");
  }
  auto request = create_run_request {
    .context = execution_context_from_host_json(*context),
    .input = body.value("input", nlohmann::json::object()),
    .metadata = body.value(
      "metadata", std::map<std::string, std::string> {}),
  };
  if (request.context.run_id.empty()) {
    throw std::invalid_argument(
      "host create_run requires a caller-assigned context.run_id");
  }
  return request;
}

[[nodiscard]] inline nlohmann::json get_run_request_to_json(
  const get_run_request& request) {
  if (request.run_id.empty()) {
    throw std::invalid_argument("get_run requires a run_id");
  }
  return { { "runId", request.run_id } };
}

[[nodiscard]] inline get_run_request get_run_request_from_json(
  const nlohmann::json& body) {
  return { .run_id = codec_detail::required_string(body, "runId") };
}

[[nodiscard]] inline nlohmann::json cancel_run_request_to_json(
  const cancel_run_request& request) {
  if (request.run_id.empty() || request.expected_revision == 0) {
    throw std::invalid_argument(
      "cancel_run requires run_id and positive expected_revision");
  }
  return {
    { "runId", request.run_id },
    { "expectedRevision", request.expected_revision },
    { "reason", request.reason },
  };
}

[[nodiscard]] inline cancel_run_request cancel_run_request_from_json(
  const nlohmann::json& body) {
  return {
    .run_id = codec_detail::required_string(body, "runId"),
    .expected_revision = codec_detail::required_revision(body),
    .reason = body.value("reason", std::string("agent run cancelled")),
  };
}

[[nodiscard]] inline nlohmann::json resolve_approval_request_to_json(
  const resolve_approval_request& request) {
  if (request.run_id.empty() || request.expected_revision == 0 ||
      request.approval_id.empty() ||
      request.resolution == runtime::approval_resolution::pending) {
    throw std::invalid_argument(
      "resolve_approval requires run identity, approval identity, and final resolution");
  }
  return {
    { "runId", request.run_id },
    { "expectedRevision", request.expected_revision },
    { "approvalId", request.approval_id },
    { "resolution", runtime::to_string(request.resolution) },
    { "reason", request.reason },
    { "metadata", request.metadata },
  };
}

[[nodiscard]] inline resolve_approval_request
resolve_approval_request_from_json(const nlohmann::json& body) {
  return {
    .run_id = codec_detail::required_string(body, "runId"),
    .expected_revision = codec_detail::required_revision(body),
    .approval_id = codec_detail::required_string(body, "approvalId"),
    .resolution = codec_detail::approval_resolution_from_string(
      codec_detail::required_string(body, "resolution")),
    .reason = body.value("reason", std::string {}),
    .metadata = body.value(
      "metadata", std::map<std::string, std::string> {}),
  };
}

[[nodiscard]] inline nlohmann::json resume_run_request_to_json(
  const resume_run_request& request) {
  if (request.run_id.empty() || request.expected_revision == 0 ||
      request.approval_id.empty()) {
    throw std::invalid_argument(
      "resume_run requires run identity and approval identity");
  }
  return {
    { "runId", request.run_id },
    { "expectedRevision", request.expected_revision },
    { "approvalId", request.approval_id },
    { "input", request.input },
  };
}

[[nodiscard]] inline resume_run_request resume_run_request_from_json(
  const nlohmann::json& body) {
  return {
    .run_id = codec_detail::required_string(body, "runId"),
    .expected_revision = codec_detail::required_revision(body),
    .approval_id = codec_detail::required_string(body, "approvalId"),
    .input = body.value("input", nlohmann::json::object()),
  };
}

[[nodiscard]] inline nlohmann::json list_events_request_to_json(
  const list_events_request& request) {
  if (request.run_id.empty() || request.limit == 0 || request.limit > 1000) {
    throw std::invalid_argument(
      "list_events requires run_id and a limit between 1 and 1000");
  }
  return {
    { "runId", request.run_id },
    { "afterSequence", request.after_sequence },
    { "limit", request.limit },
  };
}

[[nodiscard]] inline list_events_request list_events_request_from_json(
  const nlohmann::json& body) {
  const auto limit = body.value("limit", std::size_t { 256 });
  if (limit == 0 || limit > 1000) {
    throw std::invalid_argument("list_events limit must be between 1 and 1000");
  }
  return {
    .run_id = codec_detail::required_string(body, "runId"),
    .after_sequence = body.value("afterSequence", std::uint64_t {}),
    .limit = limit,
  };
}

[[nodiscard]] inline nlohmann::json run_submission_to_json(
  const run_submission& value) {
  if (value.run_id.empty() || value.revision == 0) {
    throw std::invalid_argument("invalid agent host run submission");
  }
  return {
    { "runId", value.run_id },
    { "revision", value.revision },
    { "status", runtime::to_string(value.status) },
  };
}

[[nodiscard]] inline run_submission run_submission_from_json(
  const nlohmann::json& value) {
  const auto run_id = codec_detail::required_string(value, "runId");
  const auto revision = value.value("revision", std::uint64_t {});
  if (revision == 0) {
    throw std::invalid_argument(
      "agent host run submission requires a positive revision");
  }
  return {
    .run_id = run_id,
    .revision = revision,
    .status = runtime::agent_run_status_from_string(
      codec_detail::required_string(value, "status")),
  };
}

[[nodiscard]] inline nlohmann::json approval_view_to_json(
  const approval_view& value) {
  if (value.approval_id.empty() || value.tool_call_id.empty() ||
      value.tool_name.empty()) {
    throw std::invalid_argument("invalid agent host approval view");
  }
  nlohmann::json output {
    { "approvalId", value.approval_id },
    { "toolCallId", value.tool_call_id },
    { "toolName", value.tool_name },
    { "resolution", runtime::to_string(value.resolution) },
    { "reason", value.reason },
    { "createdAtUnixMs", codec_detail::to_unix_millis(value.created_at) },
    { "metadata", codec_detail::safe_metadata(value.metadata) },
  };
  output["resolvedAtUnixMs"] = value.resolved_at
    ? nlohmann::json(codec_detail::to_unix_millis(*value.resolved_at))
    : nlohmann::json(nullptr);
  return output;
}

[[nodiscard]] inline approval_view approval_view_from_json(
  const nlohmann::json& value) {
  if (!value.is_object()) {
    throw std::invalid_argument("invalid agent host approval view");
  }
  approval_view output {
    .approval_id = codec_detail::required_string(value, "approvalId"),
    .tool_call_id = codec_detail::required_string(value, "toolCallId"),
    .tool_name = codec_detail::required_string(value, "toolName"),
    .resolution = codec_detail::approval_state_from_string(
      codec_detail::required_string(value, "resolution")),
    .reason = value.value("reason", std::string {}),
    .created_at = codec_detail::from_unix_millis(
      value.value("createdAtUnixMs", std::int64_t {})),
    .metadata = value.value(
      "metadata", std::map<std::string, std::string> {}),
  };
  if (value.contains("resolvedAtUnixMs") &&
      !value.at("resolvedAtUnixMs").is_null()) {
    output.resolved_at = codec_detail::from_unix_millis(
      value.at("resolvedAtUnixMs").get<std::int64_t>());
  }
  if (output.resolution == runtime::approval_resolution::pending &&
      output.resolved_at) {
    throw std::invalid_argument(
      "pending agent host approval cannot have a resolution timestamp");
  }
  if (output.resolution != runtime::approval_resolution::pending &&
      !output.resolved_at) {
    throw std::invalid_argument(
      "resolved agent host approval requires a resolution timestamp");
  }
  return output;
}

[[nodiscard]] inline nlohmann::json run_view_to_json(const run_view& value) {
  if (value.run_id.empty() || value.revision == 0 ||
      (!value.context.run_id.empty() && value.context.run_id != value.run_id)) {
    throw std::invalid_argument("invalid agent host run view");
  }
  nlohmann::json output {
    { "runId", value.run_id },
    { "revision", value.revision },
    { "status", runtime::to_string(value.status) },
    { "context", execution_context_to_host_json(value.context) },
    { "createdAtUnixMs", codec_detail::to_unix_millis(value.created_at) },
    { "updatedAtUnixMs", codec_detail::to_unix_millis(value.updated_at) },
    { "approval", value.approval
        ? approval_view_to_json(*value.approval)
        : nlohmann::json(nullptr) },
    { "result", value.result },
    { "error", value.error },
    { "metadata", codec_detail::safe_metadata(value.metadata) },
  };
  output["completedAtUnixMs"] = value.completed_at
    ? nlohmann::json(codec_detail::to_unix_millis(*value.completed_at))
    : nlohmann::json(nullptr);
  return output;
}

[[nodiscard]] inline run_view run_view_from_json(const nlohmann::json& value) {
  if (!value.is_object()) {
    throw std::invalid_argument("invalid agent host run view");
  }
  const auto context = value.find("context");
  if (context == value.end() || !context->is_object()) {
    throw std::invalid_argument("agent host run view requires a context");
  }
  run_view output {
    .run_id = codec_detail::required_string(value, "runId"),
    .revision = value.value("revision", std::uint64_t {}),
    .status = runtime::agent_run_status_from_string(
      codec_detail::required_string(value, "status")),
    .context = execution_context_from_host_json(*context),
    .created_at = codec_detail::from_unix_millis(
      value.value("createdAtUnixMs", std::int64_t {})),
    .updated_at = codec_detail::from_unix_millis(
      value.value("updatedAtUnixMs", std::int64_t {})),
    .result = value.value("result", nlohmann::json {}),
    .error = value.value("error", std::string {}),
    .metadata = value.value(
      "metadata", std::map<std::string, std::string> {}),
  };
  if (output.revision == 0) {
    throw std::invalid_argument(
      "agent host run view requires a positive revision");
  }
  if (output.context.run_id.empty()) {
    output.context.run_id = output.run_id;
  }
  else if (output.context.run_id != output.run_id) {
    throw std::invalid_argument(
      "agent host run context does not match the run view identity");
  }
  if (value.contains("completedAtUnixMs") &&
      !value.at("completedAtUnixMs").is_null()) {
    output.completed_at = codec_detail::from_unix_millis(
      value.at("completedAtUnixMs").get<std::int64_t>());
  }
  if (value.contains("approval") && !value.at("approval").is_null()) {
    output.approval = approval_view_from_json(value.at("approval"));
  }
  return output;
}

[[nodiscard]] inline nlohmann::json host_event_to_json(
  const host_event& value) {
  if (value.run_id.empty() || value.sequence == 0 || value.type.empty()) {
    throw std::invalid_argument("invalid agent host event");
  }
  return {
    { "runId", value.run_id },
    { "sequence", value.sequence },
    { "type", value.type },
    { "status", runtime::to_string(value.status) },
    { "stepId", value.step_id },
    { "toolCallId", value.tool_call_id },
    { "timestampUnixMs", codec_detail::to_unix_millis(value.timestamp) },
    { "data", value.data },
    { "metadata", codec_detail::safe_metadata(value.metadata) },
  };
}

[[nodiscard]] inline host_event host_event_from_json(
  const nlohmann::json& value) {
  if (!value.is_object()) {
    throw std::invalid_argument("invalid agent host event");
  }
  host_event output {
    .run_id = codec_detail::required_string(value, "runId"),
    .sequence = value.value("sequence", std::uint64_t {}),
    .type = codec_detail::required_string(value, "type"),
    .status = runtime::agent_run_status_from_string(
      codec_detail::required_string(value, "status")),
    .step_id = value.value("stepId", std::string {}),
    .tool_call_id = value.value("toolCallId", std::string {}),
    .timestamp = codec_detail::from_unix_millis(
      value.value("timestampUnixMs", std::int64_t {})),
    .data = value.value("data", nlohmann::json {}),
    .metadata = value.value(
      "metadata", std::map<std::string, std::string> {}),
  };
  if (output.sequence == 0) {
    throw std::invalid_argument(
      "agent host event requires a positive sequence");
  }
  return output;
}

[[nodiscard]] inline nlohmann::json event_page_to_json(
  const event_page& value) {
  auto events = nlohmann::json::array();
  std::uint64_t previous {};
  for (const auto& event : value.events) {
    if (event.sequence == 0 || (previous != 0 && event.sequence <= previous)) {
      throw std::invalid_argument(
        "agent host event page must have strictly increasing sequences");
    }
    previous = event.sequence;
    events.push_back(host_event_to_json(event));
  }
  if (!value.events.empty() && value.next_sequence != previous) {
    throw std::invalid_argument(
      "agent host event page cursor must equal the last event sequence");
  }
  if (value.events.empty() && value.has_more) {
    throw std::invalid_argument(
      "agent host event page cannot have more events after an empty page");
  }
  return {
    { "events", std::move(events) },
    { "nextSequence", value.next_sequence },
    { "hasMore", value.has_more },
  };
}

[[nodiscard]] inline event_page event_page_from_json(
  const nlohmann::json& value) {
  if (!value.is_object() || !value.contains("events") ||
      !value.at("events").is_array()) {
    throw std::invalid_argument("invalid agent host event page");
  }
  event_page page {
    .next_sequence = value.value("nextSequence", std::uint64_t {}),
    .has_more = value.value("hasMore", false),
  };
  std::uint64_t previous {};
  for (const auto& encoded : value.at("events")) {
    auto event = host_event_from_json(encoded);
    if (event.sequence == 0 || (previous != 0 && event.sequence <= previous)) {
      throw std::invalid_argument(
        "agent host event page must have strictly increasing sequences");
    }
    previous = event.sequence;
    page.events.push_back(std::move(event));
  }
  if (!page.events.empty() && page.next_sequence != previous) {
    throw std::invalid_argument(
      "agent host event page cursor must equal the last event sequence");
  }
  if (page.events.empty() && page.has_more) {
    throw std::invalid_argument(
      "agent host event page cannot have more events after an empty page");
  }
  return page;
}

} // namespace wuwe::agent::host

#endif // WUWE_AGENT_HOST_HOST_CODEC_HPP
