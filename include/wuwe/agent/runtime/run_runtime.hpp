#ifndef WUWE_AGENT_RUNTIME_RUN_RUNTIME_HPP
#define WUWE_AGENT_RUNTIME_RUN_RUNTIME_HPP

#include <algorithm>
#include <array>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

#include <wuwe/agent/runtime/run_store.hpp>

namespace wuwe::agent::runtime {

struct approval_claim_result {
  run_store_write_status status { run_store_write_status::not_found };
  std::uint64_t revision { 0 };
  std::optional<agent_run_suspension> continuation;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == run_store_write_status::applied && continuation.has_value();
  }
};

struct tool_result_admission {
  run_store_write_status status { run_store_write_status::not_found };
  std::uint64_t revision { 0 };
  bool duplicate { false };
  std::optional<admitted_tool_result> result;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == run_store_write_status::applied && result.has_value();
  }
};

class agent_run_runtime {
public:
  explicit agent_run_runtime(std::shared_ptr<agent_run_store> store)
      : store_(std::move(store)) {
    if (!store_) {
      throw std::invalid_argument("agent_run_runtime requires a store");
    }
    validate_agent_run_store_capabilities(store_->capabilities());
  }

  [[nodiscard]] agent_run_store_capabilities store_capabilities()
    const noexcept {
    return store_->capabilities();
  }

  [[nodiscard]] agent_run_record start(
    core::agent_execution_context context,
    std::map<std::string, std::string> metadata = {}) {
    for (const auto& [key, _] : context.metadata) {
      if (core::sensitive_execution_context_metadata_key(key)) {
        throw std::invalid_argument(
          "agent run context must not contain sensitive metadata: " + key);
      }
    }
    if (context.run_id.empty()) {
      context.run_id = make_identifier("run");
    }
    if (context.trace_id.empty()) {
      context.trace_id = make_identifier("trace");
    }
    agent_run_record record {
      .id = context.run_id,
      .status = agent_run_status::created,
      .context = std::move(context),
      .metadata = std::move(metadata),
    };
    const auto write = store_->create(record, {
      .type = "run_created",
      .status = agent_run_status::created,
    });
    if (write.status == run_store_write_status::already_exists) {
      throw std::invalid_argument("agent run already exists: " + record.id);
    }
    if (!write) {
      throw std::runtime_error("failed to create agent run: " + record.id);
    }
    record.revision = write.revision;
    return record;
  }

  [[nodiscard]] std::optional<agent_run_record> get(
    const std::string& run_id) const {
    return store_->load(run_id);
  }

  [[nodiscard]] std::vector<agent_run_event> list_events(
    const std::string& run_id,
    std::uint64_t after_sequence = 0) const {
    return store_->list_events(run_id, after_sequence);
  }

  run_store_write_result transition(
    const std::string& run_id,
    std::uint64_t expected_revision,
    agent_run_status status,
    std::string event_type,
    nlohmann::json data = {},
    std::map<std::string, std::string> metadata = {}) {
    if (event_type.empty()) {
      throw std::invalid_argument("agent run transition requires an event type");
    }
    auto record = store_->load(run_id);
    if (!record) {
      return { .status = run_store_write_status::not_found };
    }
    if (record->revision != expected_revision) {
      return {
        .status = run_store_write_status::conflict,
        .revision = record->revision,
      };
    }
    if (!valid_transition(record->status, status)) {
      throw std::logic_error(
        "invalid agent run transition from " + to_string(record->status) +
        " to " + to_string(status));
    }
    record->status = status;
    record->updated_at = std::chrono::system_clock::now();
    if (terminal(status)) {
      record->completed_at = record->updated_at;
      record->suspension.reset();
      record->active_continuation.reset();
    }
    return store_->update(expected_revision, std::move(*record), {
      .type = std::move(event_type),
      .status = status,
      .data = std::move(data),
      .metadata = std::move(metadata),
    });
  }

  run_store_write_result finish(
    const std::string& run_id,
    std::uint64_t expected_revision,
    agent_run_status status,
    nlohmann::json result,
    std::string error = {},
    std::string event_type = {}) {
    if (!terminal(status)) {
      throw std::invalid_argument("agent run finish requires a terminal status");
    }
    auto record = store_->load(run_id);
    if (!record) {
      return { .status = run_store_write_status::not_found };
    }
    if (record->revision != expected_revision) {
      return {
        .status = run_store_write_status::conflict,
        .revision = record->revision,
      };
    }
    if (!valid_transition(record->status, status)) {
      throw std::logic_error(
        "invalid terminal agent run transition from " +
        to_string(record->status) + " to " + to_string(status));
    }
    record->status = status;
    record->result = result;
    record->error = std::move(error);
    record->updated_at = std::chrono::system_clock::now();
    record->completed_at = record->updated_at;
    record->suspension.reset();
    record->active_continuation.reset();
    if (event_type.empty()) {
      event_type = "run_" + to_string(status);
    }
    return store_->update(expected_revision, std::move(*record), {
      .type = std::move(event_type),
      .status = status,
      .data = std::move(result),
    });
  }

  run_store_write_result cancel(
    const std::string& run_id,
    std::uint64_t expected_revision,
    std::string reason = "agent run cancelled") {
    return finish(
      run_id,
      expected_revision,
      agent_run_status::cancelled,
      nlohmann::json::object(),
      std::move(reason),
      "run_cancelled");
  }

  run_store_write_result append_event(
    const std::string& run_id,
    std::uint64_t expected_revision,
    std::string event_type,
    nlohmann::json data = {},
    std::string tool_call_id = {},
    std::map<std::string, std::string> metadata = {}) {
    if (event_type.empty()) {
      throw std::invalid_argument("agent run event requires a type");
    }
    auto record = store_->load(run_id);
    if (!record) {
      return { .status = run_store_write_status::not_found };
    }
    if (record->revision != expected_revision) {
      return {
        .status = run_store_write_status::conflict,
        .revision = record->revision,
      };
    }
    record->updated_at = std::chrono::system_clock::now();
    return store_->update(expected_revision, std::move(*record), {
      .type = std::move(event_type),
      .tool_call_id = std::move(tool_call_id),
      .data = std::move(data),
      .metadata = std::move(metadata),
    });
  }

  run_store_write_result suspend_for_approval(
    const std::string& run_id,
    std::uint64_t expected_revision,
    agent_run_suspension suspension) {
    if (suspension.tool_call_id.empty() || suspension.tool_name.empty()) {
      throw std::invalid_argument(
        "agent run approval suspension requires a tool call id and tool name");
    }
    auto record = store_->load(run_id);
    if (!record) {
      return { .status = run_store_write_status::not_found };
    }
    if (record->revision != expected_revision) {
      return {
        .status = run_store_write_status::conflict,
        .revision = record->revision,
      };
    }
    if (record->status != agent_run_status::running) {
      throw std::logic_error("only a running agent run can be suspended");
    }
    if (suspension.approval_id.empty()) {
      suspension.approval_id = make_identifier("approval");
    }
    if (suspension.continuation_token.empty()) {
      suspension.continuation_token = make_secret_token();
    }
    suspension.resolution = approval_resolution::pending;
    suspension.created_at = std::chrono::system_clock::now();
    record->status = agent_run_status::waiting_for_approval;
    record->suspension = suspension;
    record->active_continuation.reset();
    record->updated_at = suspension.created_at;
    return store_->update(expected_revision, std::move(*record), {
      .type = "approval_required",
      .status = agent_run_status::waiting_for_approval,
      .tool_call_id = suspension.tool_call_id,
      .data = {
        { "approval_id", suspension.approval_id },
        { "tool_name", suspension.tool_name },
        { "reason", suspension.reason },
      },
    });
  }

  run_store_write_result resolve_approval(
    const std::string& run_id,
    std::uint64_t expected_revision,
    const std::string& continuation_token,
    approval_resolution resolution,
    std::string reason = {},
    std::map<std::string, std::string> metadata = {}) {
    if (resolution == approval_resolution::pending) {
      throw std::invalid_argument("approval resolution must be approved or denied");
    }
    auto record = store_->load(run_id);
    if (!record) {
      return { .status = run_store_write_status::not_found };
    }
    if (record->revision != expected_revision) {
      return {
        .status = run_store_write_status::conflict,
        .revision = record->revision,
      };
    }
    if (record->status != agent_run_status::waiting_for_approval ||
        !record->suspension) {
      throw std::logic_error("agent run is not waiting for approval");
    }
    if (!constant_time_equal(
          record->suspension->continuation_token, continuation_token)) {
      throw std::invalid_argument("invalid continuation token");
    }
    if (record->suspension->resolution != approval_resolution::pending) {
      throw std::logic_error("approval has already been resolved");
    }
    record->suspension->resolution = resolution;
    record->suspension->reason = std::move(reason);
    record->suspension->resolved_at = std::chrono::system_clock::now();
    for (const auto& [key, value] : metadata) {
      record->suspension->metadata[key] = value;
    }
    record->updated_at = *record->suspension->resolved_at;
    if (resolution == approval_resolution::denied) {
      record->status = agent_run_status::failed;
      record->error = record->suspension->reason.empty()
        ? "tool approval denied"
        : record->suspension->reason;
      record->completed_at = record->updated_at;
    }
    const auto tool_call_id = record->suspension->tool_call_id;
    const auto approval_id = record->suspension->approval_id;
    const auto resolved_reason = record->suspension->reason;
    if (resolution == approval_resolution::denied) {
      record->suspension.reset();
      record->active_continuation.reset();
    }
    return store_->update(expected_revision, std::move(*record), {
      .type = resolution == approval_resolution::approved
                ? "approval_granted"
                : "approval_denied",
      .tool_call_id = tool_call_id,
      .data = {
        { "approval_id", approval_id },
        { "resolution", to_string(resolution) },
        { "reason", resolved_reason },
      },
    });
  }

  approval_claim_result claim_approved_continuation(
    const std::string& run_id,
    std::uint64_t expected_revision,
    const std::string& continuation_token) {
    auto record = store_->load(run_id);
    if (!record) {
      return { .status = run_store_write_status::not_found };
    }
    if (record->revision != expected_revision) {
      return {
        .status = run_store_write_status::conflict,
        .revision = record->revision,
      };
    }
    const bool first_claim =
      record->status == agent_run_status::waiting_for_approval &&
      record->suspension &&
      record->suspension->resolution == approval_resolution::approved;
    const bool recovery_claim =
      record->status == agent_run_status::running &&
      record->active_continuation &&
      record->active_continuation->resolution == approval_resolution::approved;
    if (!first_claim && !recovery_claim) {
      throw std::logic_error("agent run has no approved continuation to claim");
    }
    const auto continuation = first_claim
      ? record->suspension
      : record->active_continuation;
    if (!constant_time_equal(
          continuation->continuation_token, continuation_token)) {
      throw std::invalid_argument("invalid continuation token");
    }
    record->status = agent_run_status::running;
    record->updated_at = std::chrono::system_clock::now();
    record->active_continuation = continuation;
    record->suspension.reset();
    const auto write = store_->update(expected_revision, std::move(*record), {
      .type = first_claim ? "run_resumed" : "run_resume_reclaimed",
      .status = agent_run_status::running,
      .tool_call_id = continuation->tool_call_id,
      .data = { { "approval_id", continuation->approval_id } },
    });
    return {
      .status = write.status,
      .revision = write.revision,
      .continuation = write ? continuation : std::nullopt,
    };
  }

  tool_result_admission admit_tool_result(
    const std::string& run_id,
    std::uint64_t expected_revision,
    admitted_tool_result result) {
    auto record = store_->load(run_id);
    if (!record) {
      return { .status = run_store_write_status::not_found };
    }
    if (result.tool_call_id.empty()) {
      throw std::invalid_argument("tool result admission requires a tool call id");
    }
    const auto existing = record->admitted_tool_results.find(result.tool_call_id);
    if (existing != record->admitted_tool_results.end()) {
      if (result.idempotency_key != existing->second.idempotency_key) {
        throw std::logic_error(
          "tool call result was already admitted with a different idempotency key");
      }
      if (!result.tool_name.empty() &&
          result.tool_name != existing->second.tool_name) {
        throw std::logic_error(
          "tool call result was already admitted for a different tool");
      }
      return {
        .status = run_store_write_status::applied,
        .revision = record->revision,
        .duplicate = true,
        .result = existing->second,
      };
    }
    if (record->status != agent_run_status::running) {
      throw std::logic_error(
        "new tool results may only be admitted to a running agent run");
    }
    if (record->revision != expected_revision) {
      return {
        .status = run_store_write_status::conflict,
        .revision = record->revision,
      };
    }
    result.admitted_at = std::chrono::system_clock::now();
    record->admitted_tool_results[result.tool_call_id] = result;
    record->updated_at = result.admitted_at;
    const auto write = store_->update(expected_revision, std::move(*record), {
      .type = "tool_result_admitted",
      .tool_call_id = result.tool_call_id,
      .data = {
        { "tool_name", result.tool_name },
        { "idempotency_key", result.idempotency_key },
        { "succeeded", result.outcome.succeeded() },
        { "error_category", tools::to_string(result.outcome.error_category) },
      },
    });
    return {
      .status = write.status,
      .revision = write.revision,
      .result = write ? std::optional<admitted_tool_result>(std::move(result))
                      : std::nullopt,
    };
  }

  [[nodiscard]] static std::string make_identifier(const std::string& prefix) {
    return prefix + "-" + random_hex(16);
  }

  [[nodiscard]] static std::string make_secret_token() {
    return random_hex(32);
  }

private:
  [[nodiscard]] static bool valid_transition(
    agent_run_status from,
    agent_run_status to) noexcept {
    if (from == to) {
      return !terminal(from);
    }
    switch (from) {
      case agent_run_status::created:
        return to == agent_run_status::running ||
               to == agent_run_status::cancelled;
      case agent_run_status::running:
        return to == agent_run_status::waiting_for_approval || terminal(to);
      case agent_run_status::waiting_for_approval:
        return to == agent_run_status::running ||
               to == agent_run_status::failed ||
               to == agent_run_status::cancelled ||
               to == agent_run_status::timed_out;
      case agent_run_status::completed:
      case agent_run_status::failed:
      case agent_run_status::cancelled:
      case agent_run_status::timed_out:
        return false;
    }
    return false;
  }

  [[nodiscard]] static bool constant_time_equal(
    const std::string& lhs,
    const std::string& rhs) noexcept {
    std::size_t difference = lhs.size() ^ rhs.size();
    const auto count = (std::max)(lhs.size(), rhs.size());
    for (std::size_t index = 0; index < count; ++index) {
      const auto left = index < lhs.size()
        ? static_cast<unsigned char>(lhs[index])
        : 0U;
      const auto right = index < rhs.size()
        ? static_cast<unsigned char>(rhs[index])
        : 0U;
      difference |= left ^ right;
    }
    return difference == 0;
  }

  [[nodiscard]] static std::string random_hex(std::size_t bytes) {
    static constexpr char alphabet[] = "0123456789abcdef";
    std::random_device source;
    std::string output;
    output.reserve(bytes * 2);
    for (std::size_t index = 0; index < bytes; ++index) {
      const auto value = static_cast<unsigned int>(source());
      output.push_back(alphabet[(value >> 4U) & 0x0fU]);
      output.push_back(alphabet[value & 0x0fU]);
    }
    return output;
  }

  std::shared_ptr<agent_run_store> store_;
};

} // namespace wuwe::agent::runtime

#endif // WUWE_AGENT_RUNTIME_RUN_RUNTIME_HPP
