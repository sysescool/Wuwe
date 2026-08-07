#ifndef WUWE_AGENT_MULTI_AGENT_MULTI_AGENT_CORE_HPP
#define WUWE_AGENT_MULTI_AGENT_MULTI_AGENT_CORE_HPP

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace wuwe::agent::multi_agent {

class team_session;

enum class agent_availability {
  available,
  draining,
  offline,
};

inline std::string to_string(agent_availability value) {
  switch (value) {
    case agent_availability::available:
      return "available";
    case agent_availability::draining:
      return "draining";
    case agent_availability::offline:
      return "offline";
  }
  return "unknown";
}

struct agent_skill {
  std::string id;
  std::string name;
  std::string description;
  std::vector<std::string> tags;
  std::vector<std::string> input_modes;
  std::vector<std::string> output_modes;
  std::map<std::string, std::string> metadata;
};

struct agent_descriptor {
  std::string id;
  std::string name;
  std::string role;
  std::string description;
  std::vector<agent_skill> skills;
  std::size_t max_concurrency { 1 };
  bool remote { false };
  std::map<std::string, std::string> metadata;
};

enum class agent_message_role {
  system,
  user,
  agent,
};

inline std::string to_string(agent_message_role value) {
  switch (value) {
    case agent_message_role::system:
      return "system";
    case agent_message_role::user:
      return "user";
    case agent_message_role::agent:
      return "agent";
  }
  return "unknown";
}

struct agent_message {
  std::string id;
  agent_message_role role { agent_message_role::user };
  std::string content;
  std::string author_agent;
  std::map<std::string, std::string> metadata;
};

struct agent_artifact {
  std::string id;
  std::string name;
  std::string description;
  std::string mime_type { "text/plain" };
  std::string content;
  nlohmann::json data;
  std::map<std::string, std::string> metadata;
};

enum class agent_task_status {
  submitted,
  working,
  input_required,
  completed,
  failed,
  blocked,
  cancelled,
  timed_out,
};

inline std::string to_string(agent_task_status value) {
  switch (value) {
    case agent_task_status::submitted:
      return "submitted";
    case agent_task_status::working:
      return "working";
    case agent_task_status::input_required:
      return "input_required";
    case agent_task_status::completed:
      return "completed";
    case agent_task_status::failed:
      return "failed";
    case agent_task_status::blocked:
      return "blocked";
    case agent_task_status::cancelled:
      return "cancelled";
    case agent_task_status::timed_out:
      return "timed_out";
  }
  return "unknown";
}

enum class agent_task_error_code {
  none,
  invalid_request,
  agent_not_found,
  agent_unavailable,
  capability_not_found,
  capacity_exhausted,
  execution_failed,
  cancelled,
  timed_out,
  consensus_not_reached,
};

inline std::string to_string(agent_task_error_code value) {
  switch (value) {
    case agent_task_error_code::none:
      return "none";
    case agent_task_error_code::invalid_request:
      return "invalid_request";
    case agent_task_error_code::agent_not_found:
      return "agent_not_found";
    case agent_task_error_code::agent_unavailable:
      return "agent_unavailable";
    case agent_task_error_code::capability_not_found:
      return "capability_not_found";
    case agent_task_error_code::capacity_exhausted:
      return "capacity_exhausted";
    case agent_task_error_code::execution_failed:
      return "execution_failed";
    case agent_task_error_code::cancelled:
      return "cancelled";
    case agent_task_error_code::timed_out:
      return "timed_out";
    case agent_task_error_code::consensus_not_reached:
      return "consensus_not_reached";
  }
  return "unknown";
}

struct agent_task_request {
  std::string id;
  std::string session_id;
  std::string input;
  std::vector<agent_message> messages;
  std::vector<std::string> required_skills;
  std::string preferred_agent;
  std::map<std::string, std::string> metadata;
  std::chrono::milliseconds timeout { 0 };
};

struct agent_task_result {
  std::string task_id;
  std::string session_id;
  std::string agent_id;
  agent_task_status status { agent_task_status::completed };
  agent_task_error_code error_code { agent_task_error_code::none };
  std::string output;
  std::string error;
  std::vector<agent_message> messages;
  std::vector<agent_artifact> artifacts;
  std::map<std::string, std::string> metadata;
  std::chrono::milliseconds elapsed { 0 };
  bool detached { false };

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == agent_task_status::completed && error_code == agent_task_error_code::none;
  }
};

struct agent_execution_context {
  std::shared_ptr<team_session> session;
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

struct agent_executor_capabilities {
  bool cooperative_cancellation { false };
  bool concurrent_execution { true };
};

class agent_executor {
public:
  virtual ~agent_executor() = default;

  virtual agent_task_result execute(
    const agent_task_request& request, const agent_execution_context& context) = 0;

  [[nodiscard]] virtual agent_executor_capabilities capabilities() const noexcept {
    return {};
  }
};

class function_agent_executor final : public agent_executor {
public:
  using callback =
    std::function<agent_task_result(const agent_task_request&, const agent_execution_context&)>;

  explicit function_agent_executor(callback execute, agent_executor_capabilities capabilities = {})
      : execute_(std::move(execute)), capabilities_(capabilities) {
    if (!execute_) {
      throw std::invalid_argument("function_agent_executor requires a callback");
    }
  }

  agent_task_result execute(
    const agent_task_request& request, const agent_execution_context& context) override {
    return execute_(request, context);
  }

  [[nodiscard]] agent_executor_capabilities capabilities() const noexcept override {
    return capabilities_;
  }

private:
  callback execute_;
  agent_executor_capabilities capabilities_;
};

enum class team_event_type {
  task_submitted,
  agent_selected,
  task_started,
  task_completed,
  task_failed,
  task_blocked,
  task_cancelled,
  task_timed_out,
  consensus_started,
  consensus_completed,
  consensus_cancelled,
  consensus_failed,
};

inline std::string to_string(team_event_type value) {
  switch (value) {
    case team_event_type::task_submitted:
      return "task_submitted";
    case team_event_type::agent_selected:
      return "agent_selected";
    case team_event_type::task_started:
      return "task_started";
    case team_event_type::task_completed:
      return "task_completed";
    case team_event_type::task_failed:
      return "task_failed";
    case team_event_type::task_blocked:
      return "task_blocked";
    case team_event_type::task_cancelled:
      return "task_cancelled";
    case team_event_type::task_timed_out:
      return "task_timed_out";
    case team_event_type::consensus_started:
      return "consensus_started";
    case team_event_type::consensus_completed:
      return "consensus_completed";
    case team_event_type::consensus_cancelled:
      return "consensus_cancelled";
    case team_event_type::consensus_failed:
      return "consensus_failed";
  }
  return "unknown";
}

struct team_event {
  team_event_type type { team_event_type::task_submitted };
  std::string task_id;
  std::string session_id;
  std::string agent_id;
  std::string message;
  std::chrono::milliseconds elapsed { 0 };
  std::map<std::string, std::string> metadata;
};

using team_observer = std::function<void(const team_event&)>;

inline nlohmann::json agent_descriptor_to_json(const agent_descriptor& descriptor) {
  auto skills = nlohmann::json::array();
  for (const auto& skill : descriptor.skills) {
    skills.push_back({
      { "id", skill.id },
      { "name", skill.name },
      { "description", skill.description },
      { "tags", skill.tags },
      { "input_modes", skill.input_modes },
      { "output_modes", skill.output_modes },
      { "metadata", skill.metadata },
    });
  }
  return {
    { "id", descriptor.id },
    { "name", descriptor.name },
    { "role", descriptor.role },
    { "description", descriptor.description },
    { "skills", std::move(skills) },
    { "max_concurrency", descriptor.max_concurrency },
    { "remote", descriptor.remote },
    { "metadata", descriptor.metadata },
  };
}

inline nlohmann::json agent_task_result_to_json(const agent_task_result& result) {
  auto artifacts = nlohmann::json::array();
  for (const auto& artifact : result.artifacts) {
    artifacts.push_back({
      { "id", artifact.id },
      { "name", artifact.name },
      { "description", artifact.description },
      { "mime_type", artifact.mime_type },
      { "content", artifact.content },
      { "data", artifact.data },
      { "metadata", artifact.metadata },
    });
  }
  return {
    { "task_id", result.task_id },
    { "session_id", result.session_id },
    { "agent_id", result.agent_id },
    { "status", to_string(result.status) },
    { "error_code", to_string(result.error_code) },
    { "output", result.output },
    { "error", result.error },
    { "detached", result.detached },
    { "artifacts", std::move(artifacts) },
    { "metadata", result.metadata },
    { "elapsed_ms", result.elapsed.count() },
  };
}

} // namespace wuwe::agent::multi_agent

#endif // WUWE_AGENT_MULTI_AGENT_MULTI_AGENT_CORE_HPP
