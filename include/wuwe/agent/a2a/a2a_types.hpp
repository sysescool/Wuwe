#ifndef WUWE_AGENT_A2A_A2A_TYPES_HPP
#define WUWE_AGENT_A2A_A2A_TYPES_HPP

#include <cstddef>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace wuwe::agent::a2a {

inline constexpr std::string_view default_protocol_version = "0.3.0";

struct agent_provider {
  std::string organization;
  std::string url;
};

struct agent_capabilities {
  bool streaming { false };
  bool push_notifications { false };
  bool state_transition_history { true };
};

struct agent_skill {
  std::string id;
  std::string name;
  std::string description;
  std::vector<std::string> tags;
  std::vector<std::string> examples;
  std::vector<std::string> input_modes;
  std::vector<std::string> output_modes;
};

struct agent_card {
  std::string protocol_version { std::string(default_protocol_version) };
  std::string name;
  std::string description;
  std::string url;
  std::string version { "0.1.0" };
  std::optional<agent_provider> provider;
  std::string documentation_url;
  agent_capabilities capabilities;
  std::vector<std::string> default_input_modes { "text/plain" };
  std::vector<std::string> default_output_modes { "text/plain" };
  std::vector<agent_skill> skills;
  nlohmann::json security_schemes { nlohmann::json::object() };
  nlohmann::json security { nlohmann::json::array() };
  bool supports_authenticated_extended_card { false };
  nlohmann::json metadata { nlohmann::json::object() };
};

enum class part_kind {
  text,
  data,
  file,
};

inline std::string to_string(part_kind value) {
  switch (value) {
    case part_kind::text: return "text";
    case part_kind::data: return "data";
    case part_kind::file: return "file";
  }
  return "unknown";
}

struct file_part {
  std::string name;
  std::string mime_type;
  std::string uri;
  std::string bytes_base64;
};

struct part {
  part_kind kind { part_kind::text };
  std::string text;
  nlohmann::json data;
  file_part file;
  nlohmann::json metadata { nlohmann::json::object() };

  static part text_part(std::string value) {
    return { .kind = part_kind::text, .text = std::move(value) };
  }

  static part data_part(nlohmann::json value) {
    return { .kind = part_kind::data, .data = std::move(value) };
  }
};

enum class message_role {
  user,
  agent,
};

inline std::string to_string(message_role value) {
  return value == message_role::user ? "user" : "agent";
}

struct message {
  std::string message_id;
  message_role role { message_role::user };
  std::vector<part> parts;
  std::string task_id;
  std::string context_id;
  std::vector<std::string> reference_task_ids;
  nlohmann::json metadata { nlohmann::json::object() };
};

enum class task_state {
  submitted,
  working,
  input_required,
  completed,
  canceled,
  failed,
  rejected,
  auth_required,
  unknown,
};

inline std::string to_string(task_state value) {
  switch (value) {
    case task_state::submitted: return "submitted";
    case task_state::working: return "working";
    case task_state::input_required: return "input-required";
    case task_state::completed: return "completed";
    case task_state::canceled: return "canceled";
    case task_state::failed: return "failed";
    case task_state::rejected: return "rejected";
    case task_state::auth_required: return "auth-required";
    case task_state::unknown: return "unknown";
  }
  return "unknown";
}

struct task_status {
  task_state state { task_state::submitted };
  std::optional<message> status_message;
  std::string timestamp;
};

struct artifact {
  std::string artifact_id;
  std::string name;
  std::string description;
  std::vector<part> parts;
  nlohmann::json metadata { nlohmann::json::object() };
};

struct task {
  std::string id;
  std::string context_id;
  task_status status;
  std::vector<artifact> artifacts;
  std::vector<message> history;
  nlohmann::json metadata { nlohmann::json::object() };
};

struct send_message_configuration {
  std::vector<std::string> accepted_output_modes;
  std::size_t history_length { 0 };
  bool blocking { true };
};

struct send_message_params {
  message value;
  send_message_configuration configuration;
  nlohmann::json metadata { nlohmann::json::object() };
};

struct task_query_params {
  std::string id;
  std::size_t history_length { 0 };
  nlohmann::json metadata { nlohmann::json::object() };
};

struct task_id_params {
  std::string id;
  nlohmann::json metadata { nlohmann::json::object() };
};

enum class error_code : int {
  parse_error = -32700,
  invalid_request = -32600,
  method_not_found = -32601,
  invalid_params = -32602,
  internal_error = -32603,
  task_not_found = -32001,
  task_not_cancelable = -32002,
  push_notification_not_supported = -32003,
  unsupported_operation = -32004,
  content_type_not_supported = -32005,
  invalid_agent_response = -32006,
  transport_error = -32050,
};

struct error {
  error_code code { error_code::internal_error };
  std::string message;
  std::optional<nlohmann::json> data;
};

struct rpc_result {
  nlohmann::json value;
  std::optional<error> failure;

  [[nodiscard]] explicit operator bool() const noexcept {
    return !failure.has_value();
  }
};

template<typename T>
struct result {
  std::optional<T> value;
  std::optional<error> failure;

  [[nodiscard]] explicit operator bool() const noexcept {
    return value.has_value() && !failure.has_value();
  }
};

inline nlohmann::json to_json(const part& value) {
  nlohmann::json output {
    { "kind", to_string(value.kind) },
    { "metadata", value.metadata },
  };
  switch (value.kind) {
    case part_kind::text:
      output["text"] = value.text;
      break;
    case part_kind::data:
      output["data"] = value.data;
      break;
    case part_kind::file:
      output["file"] = {
        { "name", value.file.name },
        { "mimeType", value.file.mime_type },
      };
      if (!value.file.uri.empty()) {
        output["file"]["uri"] = value.file.uri;
      }
      if (!value.file.bytes_base64.empty()) {
        output["file"]["bytes"] = value.file.bytes_base64;
      }
      break;
  }
  return output;
}

inline nlohmann::json to_json(const message& value) {
  auto parts = nlohmann::json::array();
  for (const auto& item : value.parts) {
    parts.push_back(to_json(item));
  }
  nlohmann::json output {
    { "kind", "message" },
    { "messageId", value.message_id },
    { "role", to_string(value.role) },
    { "parts", std::move(parts) },
    { "metadata", value.metadata },
  };
  if (!value.task_id.empty()) output["taskId"] = value.task_id;
  if (!value.context_id.empty()) output["contextId"] = value.context_id;
  if (!value.reference_task_ids.empty()) {
    output["referenceTaskIds"] = value.reference_task_ids;
  }
  return output;
}

inline nlohmann::json to_json(const artifact& value) {
  auto parts = nlohmann::json::array();
  for (const auto& item : value.parts) {
    parts.push_back(to_json(item));
  }
  return {
    { "kind", "artifact" },
    { "artifactId", value.artifact_id },
    { "name", value.name },
    { "description", value.description },
    { "parts", std::move(parts) },
    { "metadata", value.metadata },
  };
}

inline nlohmann::json to_json(const task_status& value) {
  nlohmann::json output {
    { "state", to_string(value.state) },
  };
  if (value.status_message) output["message"] = to_json(*value.status_message);
  if (!value.timestamp.empty()) output["timestamp"] = value.timestamp;
  return output;
}

inline nlohmann::json to_json(const task& value) {
  auto artifacts = nlohmann::json::array();
  for (const auto& item : value.artifacts) artifacts.push_back(to_json(item));
  auto history = nlohmann::json::array();
  for (const auto& item : value.history) history.push_back(to_json(item));
  return {
    { "kind", "task" },
    { "id", value.id },
    { "contextId", value.context_id },
    { "status", to_json(value.status) },
    { "artifacts", std::move(artifacts) },
    { "history", std::move(history) },
    { "metadata", value.metadata },
  };
}

inline nlohmann::json to_json(const agent_card& value) {
  auto skills = nlohmann::json::array();
  for (const auto& skill : value.skills) {
    skills.push_back({
      { "id", skill.id },
      { "name", skill.name },
      { "description", skill.description },
      { "tags", skill.tags },
      { "examples", skill.examples },
      { "inputModes", skill.input_modes },
      { "outputModes", skill.output_modes },
    });
  }
  nlohmann::json output {
    { "protocolVersion", value.protocol_version },
    { "name", value.name },
    { "description", value.description },
    { "url", value.url },
    { "version", value.version },
    { "capabilities", {
      { "streaming", value.capabilities.streaming },
      { "pushNotifications", value.capabilities.push_notifications },
      { "stateTransitionHistory", value.capabilities.state_transition_history },
    } },
    { "defaultInputModes", value.default_input_modes },
    { "defaultOutputModes", value.default_output_modes },
    { "skills", std::move(skills) },
    { "securitySchemes", value.security_schemes },
    { "security", value.security },
    { "supportsAuthenticatedExtendedCard", value.supports_authenticated_extended_card },
    { "metadata", value.metadata },
  };
  if (value.provider) {
    output["provider"] = {
      { "organization", value.provider->organization },
      { "url", value.provider->url },
    };
  }
  if (!value.documentation_url.empty()) {
    output["documentationUrl"] = value.documentation_url;
  }
  return output;
}

inline part part_from_json(const nlohmann::json& value) {
  part output;
  const auto kind = value.value("kind", "text");
  if (kind == "text") {
    output.kind = part_kind::text;
    output.text = value.value("text", "");
  }
  else if (kind == "data") {
    output.kind = part_kind::data;
    output.data = value.value("data", nlohmann::json());
  }
  else if (kind == "file") {
    output.kind = part_kind::file;
    const auto file = value.value("file", nlohmann::json::object());
    output.file.name = file.value("name", "");
    output.file.mime_type = file.value("mimeType", "");
    output.file.uri = file.value("uri", "");
    output.file.bytes_base64 = file.value("bytes", "");
  }
  else {
    throw std::invalid_argument("unsupported A2A part kind: " + kind);
  }
  output.metadata = value.value("metadata", nlohmann::json::object());
  return output;
}

inline message message_from_json(const nlohmann::json& value) {
  message output;
  output.message_id = value.value("messageId", "");
  const auto role = value.value("role", "user");
  if (role == "user") output.role = message_role::user;
  else if (role == "agent") output.role = message_role::agent;
  else throw std::invalid_argument("unsupported A2A message role: " + role);
  for (const auto& item : value.value("parts", nlohmann::json::array())) {
    output.parts.push_back(part_from_json(item));
  }
  output.task_id = value.value("taskId", "");
  output.context_id = value.value("contextId", "");
  output.reference_task_ids =
    value.value("referenceTaskIds", std::vector<std::string> {});
  output.metadata = value.value("metadata", nlohmann::json::object());
  if (output.message_id.empty() || output.parts.empty()) {
    throw std::invalid_argument("A2A message requires messageId and parts");
  }
  return output;
}

inline task_state task_state_from_string(std::string_view value) {
  if (value == "submitted") return task_state::submitted;
  if (value == "working") return task_state::working;
  if (value == "input-required") return task_state::input_required;
  if (value == "completed") return task_state::completed;
  if (value == "canceled") return task_state::canceled;
  if (value == "failed") return task_state::failed;
  if (value == "rejected") return task_state::rejected;
  if (value == "auth-required") return task_state::auth_required;
  return task_state::unknown;
}

inline artifact artifact_from_json(const nlohmann::json& value) {
  artifact output;
  output.artifact_id = value.value("artifactId", "");
  output.name = value.value("name", "");
  output.description = value.value("description", "");
  for (const auto& item : value.value("parts", nlohmann::json::array())) {
    output.parts.push_back(part_from_json(item));
  }
  output.metadata = value.value("metadata", nlohmann::json::object());
  return output;
}

inline task task_from_json(const nlohmann::json& value) {
  task output;
  output.id = value.value("id", "");
  output.context_id = value.value("contextId", "");
  const auto status = value.value("status", nlohmann::json::object());
  output.status.state = task_state_from_string(status.value("state", "unknown"));
  output.status.timestamp = status.value("timestamp", "");
  if (status.contains("message")) {
    output.status.status_message = message_from_json(status.at("message"));
  }
  for (const auto& item : value.value("artifacts", nlohmann::json::array())) {
    output.artifacts.push_back(artifact_from_json(item));
  }
  for (const auto& item : value.value("history", nlohmann::json::array())) {
    output.history.push_back(message_from_json(item));
  }
  output.metadata = value.value("metadata", nlohmann::json::object());
  if (output.id.empty()) {
    throw std::invalid_argument("A2A task requires an id");
  }
  return output;
}

inline agent_card agent_card_from_json(const nlohmann::json& value) {
  agent_card output;
  output.protocol_version = value.value("protocolVersion", std::string(default_protocol_version));
  output.name = value.value("name", "");
  output.description = value.value("description", "");
  output.url = value.value("url", "");
  output.version = value.value("version", "");
  if (value.contains("provider")) {
    output.provider = agent_provider {
      .organization = value.at("provider").value("organization", ""),
      .url = value.at("provider").value("url", ""),
    };
  }
  output.documentation_url = value.value("documentationUrl", "");
  const auto capabilities = value.value("capabilities", nlohmann::json::object());
  output.capabilities.streaming = capabilities.value("streaming", false);
  output.capabilities.push_notifications =
    capabilities.value("pushNotifications", false);
  output.capabilities.state_transition_history =
    capabilities.value("stateTransitionHistory", true);
  output.default_input_modes =
    value.value("defaultInputModes", std::vector<std::string> { "text/plain" });
  output.default_output_modes =
    value.value("defaultOutputModes", std::vector<std::string> { "text/plain" });
  for (const auto& item : value.value("skills", nlohmann::json::array())) {
    output.skills.push_back({
      .id = item.value("id", ""),
      .name = item.value("name", ""),
      .description = item.value("description", ""),
      .tags = item.value("tags", std::vector<std::string> {}),
      .examples = item.value("examples", std::vector<std::string> {}),
      .input_modes = item.value("inputModes", std::vector<std::string> {}),
      .output_modes = item.value("outputModes", std::vector<std::string> {}),
    });
  }
  output.security_schemes = value.value("securitySchemes", nlohmann::json::object());
  output.security = value.value("security", nlohmann::json::array());
  output.supports_authenticated_extended_card =
    value.value("supportsAuthenticatedExtendedCard", false);
  output.metadata = value.value("metadata", nlohmann::json::object());
  if (output.name.empty() || output.url.empty()) {
    throw std::invalid_argument("A2A Agent Card requires name and url");
  }
  return output;
}

} // namespace wuwe::agent::a2a

#endif // WUWE_AGENT_A2A_A2A_TYPES_HPP
