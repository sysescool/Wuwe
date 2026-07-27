#ifndef WUWE_AGENT_HOST_HOST_SERVICE_HPP
#define WUWE_AGENT_HOST_HOST_SERVICE_HPP

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/core/execution_context.hpp>
#include <wuwe/agent/host/host_protocol.hpp>
#include <wuwe/agent/runtime/run_types.hpp>

namespace wuwe::agent::host {

template<typename T>
class host_result {
public:
  [[nodiscard]] explicit operator bool() const noexcept {
    return std::holds_alternative<T>(storage_);
  }

  [[nodiscard]] static host_result success(T value) {
    return host_result(std::move(value));
  }

  [[nodiscard]] static host_result failure(host_error error) {
    return host_result(std::move(error));
  }

  [[nodiscard]] const T* value_if() const noexcept {
    return std::get_if<T>(&storage_);
  }

  [[nodiscard]] T* value_if() noexcept {
    return std::get_if<T>(&storage_);
  }

  [[nodiscard]] const host_error* error_if() const noexcept {
    return std::get_if<host_error>(&storage_);
  }

private:
  explicit host_result(T value) : storage_(std::move(value)) {}
  explicit host_result(host_error error) : storage_(std::move(error)) {}

  std::variant<T, host_error> storage_;
};

struct create_run_request {
  core::agent_execution_context context;
  nlohmann::json input = nlohmann::json::object();
  std::map<std::string, std::string> metadata;
};

struct get_run_request {
  std::string run_id;
};

struct cancel_run_request {
  std::string run_id;
  std::uint64_t expected_revision { 0 };
  std::string reason;
};

struct resolve_approval_request {
  std::string run_id;
  std::uint64_t expected_revision { 0 };
  std::string approval_id;
  runtime::approval_resolution resolution { runtime::approval_resolution::pending };
  std::string reason;
  std::map<std::string, std::string> metadata;
};

struct resume_run_request {
  std::string run_id;
  std::uint64_t expected_revision { 0 };
  std::string approval_id;
  nlohmann::json input = nlohmann::json::object();
};

struct list_events_request {
  std::string run_id;
  std::uint64_t after_sequence { 0 };
  std::size_t limit { 256 };
};

struct run_submission {
  std::string run_id;
  std::uint64_t revision { 0 };
  runtime::agent_run_status status { runtime::agent_run_status::created };
};

struct approval_view {
  std::string approval_id;
  std::string tool_call_id;
  std::string tool_name;
  runtime::approval_resolution resolution { runtime::approval_resolution::pending };
  std::string reason;
  std::chrono::system_clock::time_point created_at {};
  std::optional<std::chrono::system_clock::time_point> resolved_at;
  std::map<std::string, std::string> metadata;
};

struct run_view {
  std::string run_id;
  std::uint64_t revision { 0 };
  runtime::agent_run_status status { runtime::agent_run_status::created };
  core::agent_execution_context context;
  std::chrono::system_clock::time_point created_at {};
  std::chrono::system_clock::time_point updated_at {};
  std::optional<std::chrono::system_clock::time_point> completed_at;
  std::optional<approval_view> approval;
  nlohmann::json result;
  std::string error;
  std::map<std::string, std::string> metadata;
};

struct host_event {
  std::string run_id;
  std::uint64_t sequence { 0 };
  std::string type;
  runtime::agent_run_status status { runtime::agent_run_status::created };
  std::string step_id;
  std::string tool_call_id;
  std::chrono::system_clock::time_point timestamp {};
  nlohmann::json data;
  std::map<std::string, std::string> metadata;
};

struct event_page {
  std::vector<host_event> events;
  std::uint64_t next_sequence { 0 };
  bool has_more { false };
};

[[nodiscard]] inline approval_view approval_view_from_runtime(
  const runtime::agent_run_suspension& value) {
  return {
    .approval_id = value.approval_id,
    .tool_call_id = value.tool_call_id,
    .tool_name = value.tool_name,
    .resolution = value.resolution,
    .reason = value.reason,
    .created_at = value.created_at,
    .resolved_at = value.resolved_at,
    .metadata = value.metadata,
  };
}

[[nodiscard]] inline run_view run_view_from_runtime(
  const runtime::agent_run_record& value) {
  const auto* approval = value.suspension
    ? &*value.suspension
    : (value.active_continuation ? &*value.active_continuation : nullptr);
  return {
    .run_id = value.id,
    .revision = value.revision,
    .status = value.status,
    .context = value.context,
    .created_at = value.created_at,
    .updated_at = value.updated_at,
    .completed_at = value.completed_at,
    .approval = approval
      ? std::optional<approval_view>(approval_view_from_runtime(*approval))
      : std::nullopt,
    .result = value.result,
    .error = value.error,
    .metadata = value.metadata,
  };
}

[[nodiscard]] inline host_event host_event_from_runtime(
  const runtime::agent_run_event& value) {
  return {
    .run_id = value.run_id,
    .sequence = value.sequence,
    .type = value.type,
    .status = value.status,
    .step_id = value.step_id,
    .tool_call_id = value.tool_call_id,
    .timestamp = value.timestamp,
    .data = value.data,
    .metadata = value.metadata,
  };
}

class agent_host_service {
public:
  virtual ~agent_host_service() = default;

  [[nodiscard]] virtual host_result<run_submission> create_run(
    const host_call_context& call,
    const create_run_request& request) = 0;
  [[nodiscard]] virtual host_result<run_view> get_run(
    const host_call_context& call,
    const get_run_request& request) = 0;
  [[nodiscard]] virtual host_result<run_submission> cancel_run(
    const host_call_context& call,
    const cancel_run_request& request) = 0;
  [[nodiscard]] virtual host_result<run_submission> resolve_approval(
    const host_call_context& call,
    const resolve_approval_request& request) = 0;
  [[nodiscard]] virtual host_result<run_submission> resume_run(
    const host_call_context& call,
    const resume_run_request& request) = 0;
  [[nodiscard]] virtual host_result<event_page> list_events(
    const host_call_context& call,
    const list_events_request& request) = 0;
};

} // namespace wuwe::agent::host

#endif // WUWE_AGENT_HOST_HOST_SERVICE_HPP
