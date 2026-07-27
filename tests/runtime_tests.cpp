#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <wuwe/agent/llm/llm_agent_runner.h>
#include <wuwe/agent/runtime/runtime.hpp>
#include <wuwe/agent/tools/tool.hpp>
#include <wuwe/common/print.h>

namespace runtime_test_tools {

inline std::atomic<int> executions { 0 };

struct guarded_write {
  static constexpr std::string_view description =
    "Write a named value after explicit approval.";

  std::string name;
  std::string value;

  std::string invoke() const {
    ++executions;
    return name + "=" + value;
  }
};

struct read_value {
  static constexpr std::string_view description = "Read a stable value.";

  std::string name;

  std::string invoke() const {
    ++executions;
    return "value:" + name;
  }
};

struct slow_read {
  static constexpr std::string_view description = "Read a value slowly.";

  std::string name;

  std::string invoke() const {
    ++executions;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return "late:" + name;
  }
};

struct invalid_contract_tool {
  static constexpr std::string_view description = "Invalid contract fixture.";

  std::string invoke() const { return "unused"; }
};

} // namespace runtime_test_tools

namespace wuwe {

template<>
struct tool_contract<runtime_test_tools::guarded_write> {
  static agent::tools::tool_descriptor descriptor() {
    auto value = agent::tools::descriptor_from_llm_tool(
      make_llm_tool<runtime_test_tools::guarded_write>());
    value.version = "2";
    value.side_effect = agent::tools::tool_side_effect::write;
    value.idempotency = agent::tools::tool_idempotency::idempotent_with_key;
    value.approval = agent::tools::tool_approval_mode::always;
    value.capabilities.push_back({
      .name = agent::capability::names::filesystem_write,
      .risk = agent::capability::capability_risk_level::high,
      .summary = "write protected state",
    });
    return value;
  }
};

template<>
struct tool_contract<runtime_test_tools::slow_read> {
  static agent::tools::tool_descriptor descriptor() {
    auto value = agent::tools::descriptor_from_llm_tool(
      make_llm_tool<runtime_test_tools::slow_read>());
    value.timeout = std::chrono::milliseconds(20);
    return value;
  }
};

template<>
struct tool_contract<runtime_test_tools::invalid_contract_tool> {
  static agent::tools::tool_descriptor descriptor() {
    return {
      .name = "invalid_contract_tool",
      .input_schema = nlohmann::json::array(),
    };
  }
};

} // namespace wuwe

namespace {

using namespace wuwe;
using namespace wuwe::agent;
using namespace runtime_test_tools;

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class approval_then_final_client final : public llm_client {
public:
  llm_response complete(const llm_request& request) override {
    requests.push_back(request);
    if (requests.size() == 1) {
      return {
        .content = "I will update the value.",
        .usage = { .prompt_tokens = 8, .completion_tokens = 2,
          .total_tokens = 10 },
        .tool_calls = { {
          .id = "write-1",
          .name = "guarded_write",
          .arguments_json = R"({"name":"mode","value":"strict"})",
        } },
      };
    }
    return {
      .content = "Update completed.",
      .usage = { .prompt_tokens = 5, .completion_tokens = 1,
        .total_tokens = 6 },
    };
  }

  std::vector<llm_request> requests;
};

class two_approvals_then_final_client final : public llm_client {
public:
  llm_response complete(const llm_request& request) override {
    requests.push_back(request);
    if (requests.size() == 1) {
      return {
        .content = "I will update both values.",
        .tool_calls = {
          {
            .id = "write-a",
            .name = "guarded_write",
            .arguments_json = R"({"name":"mode","value":"strict"})",
          },
          {
            .id = "write-b",
            .name = "guarded_write",
            .arguments_json = R"({"name":"audit","value":"enabled"})",
          },
        },
      };
    }
    return { .content = "Both updates completed." };
  }

  std::vector<llm_request> requests;
};

class manual_approval_service final : public approval::approval_service {
public:
  approval::approval_decision decide(
    const approval::approval_request& request) override {
    last_request = request;
    return {
      .kind = approval::approval_decision_kind::needs_manual_review,
      .reason = "operator review required",
    };
  }

  approval::approval_request last_request;
};

class metered_tool_loop_client final : public llm_client {
public:
  explicit metered_tool_loop_client(
    std::string tool_name = "read_value",
    std::string call_id = "read-1")
      : tool_name_(std::move(tool_name)), call_id_(std::move(call_id)) {}

  llm_response complete(const llm_request& request) override {
    requests.push_back(request);
    if (requests.size() % 2 == 1) {
      return {
        .content = "reading",
        .usage = { .prompt_tokens = 10, .completion_tokens = 4,
          .total_tokens = 14, .cached_prompt_tokens = 2,
          .reasoning_tokens = 1 },
        .tool_calls = { {
          .id = call_id_,
          .name = tool_name_,
          .arguments_json = R"({"name":"mode"})",
        } },
      };
    }
    return {
      .content = "done",
      .usage = { .prompt_tokens = 6, .completion_tokens = 2,
        .total_tokens = 8, .cached_prompt_tokens = 1 },
    };
  }

  std::vector<llm_request> requests;

private:
  std::string tool_name_;
  std::string call_id_;
};

class repeated_call_id_client final : public llm_client {
public:
  llm_response complete(const llm_request& request) override {
    requests.push_back(request);
    return {
      .content = "read again",
      .tool_calls = { {
        .id = "reused-call",
        .name = "read_value",
        .arguments_json = R"({"name":"mode"})",
      } },
    };
  }

  std::vector<llm_request> requests;
};

class unknown_tool_client final : public llm_client {
public:
  llm_response complete(const llm_request&) override {
    return {
      .tool_calls = { {
        .id = "unknown-1",
        .name = "unregistered_tool",
        .arguments_json = "{}",
      } },
    };
  }
};

void execution_context_round_trips_without_runtime_only_state() {
  core::agent_execution_context context {
    .run_id = "run-1",
    .trace_id = "trace-1",
    .request_id = "request-1",
    .tenant_id = "tenant-1",
    .user_id = "user-1",
    .application_id = "app-1",
    .workspace_id = "workspace-1",
    .conversation_id = "conversation-1",
    .agent_id = "agent-1",
    .locale = "zh-CN",
    .deadline = std::chrono::system_clock::now() + std::chrono::minutes(1),
    .metadata = { { "region", "cn" } },
  };
  const auto restored = core::execution_context_from_json(
    core::execution_context_to_json(context));
  require(restored.run_id == context.run_id &&
      restored.trace_id == context.trace_id &&
      restored.tenant_id == context.tenant_id &&
      restored.metadata == context.metadata &&
      restored.deadline.has_value(),
    "execution context should preserve durable identity and deadline fields");
  require(!restored.stop_token.stop_possible(),
    "serialized execution contexts must not pretend to persist cancellation tokens");
}

void run_store_enforces_optimistic_concurrency_and_idempotency() {
  auto store = std::make_shared<runtime::in_memory_agent_run_store>();
  runtime::agent_run_runtime runtime(store);
  auto record = runtime.start({ .run_id = "optimistic-run" });
  const auto running = runtime.transition(
    record.id, record.revision, runtime::agent_run_status::running, "run_started");
  require(running && running.revision == 2,
    "starting a run should advance its revision");
  const auto stale = runtime.transition(
    record.id, record.revision, runtime::agent_run_status::cancelled, "stale_cancel");
  require(stale.status == runtime::run_store_write_status::conflict &&
      stale.revision == running.revision,
    "stale writes should return the current revision without mutation");

  auto admitted = runtime.admit_tool_result(record.id, running.revision, {
    .tool_call_id = "call-1",
    .idempotency_key = "key-1",
    .tool_name = "write",
    .outcome = { .content = "ok" },
  });
  require(admitted && !admitted.duplicate,
    "the first tool result should be admitted");
  auto duplicate = runtime.admit_tool_result(record.id, running.revision, {
    .tool_call_id = "call-1",
    .idempotency_key = "key-1",
    .tool_name = "write",
    .outcome = { .content = "different" },
  });
  require(duplicate && duplicate.duplicate &&
      duplicate.revision == admitted.revision &&
      duplicate.result->outcome.content == "ok",
    "a stale duplicate admission should return the original result without a new event");

  const auto cancelled = runtime.cancel(
    record.id, admitted.revision, "cancelled by operator");
  require(cancelled &&
      runtime.get(record.id)->status == runtime::agent_run_status::cancelled,
    "the runtime should expose an optimistic durable cancellation operation");

  bool repeated_finish_rejected = false;
  try {
    (void)runtime.finish(record.id, cancelled.revision,
      runtime::agent_run_status::cancelled, nlohmann::json::object());
  }
  catch (const std::logic_error&) {
    repeated_finish_rejected = true;
  }
  require(repeated_finish_rejected,
    "terminal runs must reject repeated terminal writes");

  bool late_result_rejected = false;
  try {
    (void)runtime.admit_tool_result(record.id, cancelled.revision, {
      .tool_call_id = "call-2",
      .idempotency_key = "key-2",
      .tool_name = "write",
      .outcome = { .content = "late" },
    });
  }
  catch (const std::logic_error&) {
    late_result_rejected = true;
  }
  require(late_result_rejected,
    "terminal runs must reject newly arriving tool results");
}

void persisted_run_payloads_validate_schema_and_error_codes() {
  runtime::agent_run_record record {
    .id = "serialized-run",
    .admitted_tool_results = { {
      "call-1",
      {
        .tool_call_id = "call-1",
        .tool_name = "read",
        .outcome = {
          .content = "timed out",
          .error_code = std::make_error_code(std::errc::timed_out),
          .error_category = tools::tool_error_category::timeout,
          .retryable = true,
        },
      },
    } },
  };
  const auto restored = runtime::agent_run_record_from_json(
    runtime::agent_run_record_to_json(record));
  require(restored.admitted_tool_results.at("call-1").outcome.error_code ==
      std::errc::timed_out,
    "standard tool error codes should survive durable round trips");

  auto unsupported = runtime::agent_run_record_to_json(record);
  unsupported["schema_version"] = 99;
  bool rejected = false;
  try {
    (void)runtime::agent_run_record_from_json(unsupported);
  }
  catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "unsupported durable run schemas should fail explicitly");

  auto invalid_status = runtime::agent_run_record_to_json(record);
  invalid_status["status"] = "future_status";
  rejected = false;
  try { (void)runtime::agent_run_record_from_json(invalid_status); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "unknown durable run statuses should fail explicitly");

  const auto suspension_json = nlohmann::json {
    { "approval_id", "approval-1" },
    { "continuation_token", "token-1" },
    { "tool_call_id", "call-1" },
    { "tool_name", "read_value" },
    { "resolution", "approved" },
    { "continuation", nlohmann::json::object() },
    { "created_at_unix_ms", 1 },
    { "resolved_at_unix_ms", nullptr },
  };
  rejected = false;
  try { (void)runtime::run_suspension_from_json(suspension_json); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected,
    "resolved durable suspensions should require a resolution timestamp");

  rejected = false;
  try {
    (void)runtime::agent_run_event_from_json({
      { "schema_version", 1 }, { "run_id", "serialized-run" },
      { "sequence", 0 }, { "type", "run_created" },
      { "status", "created" }, { "timestamp_unix_ms", 1 },
    });
  }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "durable events should require a positive replay sequence");

  runtime::llm_tool_continuation advanced_continuation;
  advanced_continuation.request.stop_sequences = { "END" };
  advanced_continuation.request.seed = 77;
  advanced_continuation.request.json_schema_output = {
    .name = "result",
    .schema = { { "type", "object" } },
  };
  advanced_continuation.request.cache_mode = wuwe::llm_cache_mode::enabled;
  const auto restored_continuation = runtime::llm_continuation_from_json(
    runtime::llm_continuation_to_json(advanced_continuation));
  require(restored_continuation.request.stop_sequences ==
            std::vector<std::string> { "END" } &&
          restored_continuation.request.seed == 77 &&
          restored_continuation.request.json_schema_output &&
          restored_continuation.request.json_schema_output->name == "result" &&
          restored_continuation.request.cache_mode == wuwe::llm_cache_mode::enabled,
    "durable continuations should preserve advanced generation controls");

  auto invalid_continuation = runtime::llm_continuation_to_json({
    .pending_calls = { {
      .id = "pending-1", .name = "read_value", .arguments_json = "{}",
    } },
    .approved_call_ids = { "not-pending" },
  });
  rejected = false;
  try { (void)runtime::llm_continuation_from_json(invalid_continuation); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected,
    "durable continuations should reject approvals outside the pending batch");

  auto invalid_pricing = runtime::llm_continuation_to_json({});
  invalid_pricing["pricing"] = {
    { "input_per_million_tokens_usd", -1.0 },
    { "output_per_million_tokens_usd", 1.0 },
  };
  rejected = false;
  try { (void)runtime::llm_continuation_from_json(invalid_pricing); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "durable continuations should reject invalid pricing");
}

void tool_contracts_reject_invalid_descriptors() {
  bool rejected = false;
  try { (void)make_tool_descriptor<invalid_contract_tool>(); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected,
    "invalid contract schemas must not be silently replaced by reflection defaults");

  rejected = false;
  try {
    (void)tools::descriptor_from_llm_tool({
      .name = "invalid_json_tool",
      .parameters_json_schema = "{not-json}",
    });
  }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "invalid model-facing JSON schemas should fail explicitly");
}

void runner_rejects_unregistered_and_reused_tool_calls() {
  executions = 0;
  auto provider = std::make_shared<tool_provider<read_value>>();
  unknown_tool_client unknown_client;
  llm_agent_runner unknown_runner(unknown_client, provider);
  llm_agent_run_options options;
  options.callbacks.prepare_model_request = [](llm_request request) {
    request.tools.push_back({
      .name = "unregistered_tool",
      .parameters_json_schema = R"({"type":"object"})",
    });
    return std::optional<llm_request>(std::move(request));
  };
  const auto unknown = unknown_runner.complete("run unknown", options);
  require(unknown.error_code == agent::llm_error_code::tool_call_denied &&
      executions == 0,
    "the provider descriptor registry must remain authoritative after request preparation");

  repeated_call_id_client repeated_client;
  llm_agent_runner repeated_runner(repeated_client, provider, 3);
  const auto repeated = repeated_runner.complete("read twice");
  require(repeated.error_code == agent::llm_error_code::invalid_response &&
      repeated.stop_reason == "invalid_tool_call" && executions == 1,
    "tool call ids must not be reusable across non-durable model rounds");
}

void runner_enforces_tool_deadlines_without_admitting_late_results() {
  executions = 0;
  metered_tool_loop_client client("slow_read", "slow-1");
  auto provider = std::make_shared<tool_provider<slow_read>>();
  llm_agent_runner runner(client, provider);
  llm_agent_run_options options;
  options.max_in_flight_tool_invocations = 1;
  const auto started = std::chrono::steady_clock::now();
  const auto response = runner.complete("read slowly", options);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  require(!response.error_code && response.content == "done" &&
      elapsed < std::chrono::milliseconds(150),
    "tool descriptor deadlines should return control before a non-cooperative tool finishes");
  require(client.requests.size() == 2 &&
      nlohmann::json::parse(client.requests.back().messages.back().content)
        ["error"]["category"] == "timeout",
    "late tool output must be discarded and represented as a timeout outcome");

  const auto saturated = runner.complete("read slowly again", options);
  require(!saturated.error_code && saturated.content == "done" &&
      client.requests.size() == 4 &&
      nlohmann::json::parse(client.requests.back().messages.back().content)
        ["error"]["category"] == "unavailable" && executions == 1,
    "unfinished timed-out tools should retain a shared capacity slot and bound detachment");
  std::this_thread::sleep_for(std::chrono::milliseconds(210));
}

void runner_aggregates_usage_cost_and_preserves_execution_context() {
  executions = 0;
  metered_tool_loop_client client;
  auto provider = std::make_shared<tool_provider<read_value>>();
  llm_agent_runner runner(client, provider);
  auto store = std::make_shared<runtime::in_memory_agent_run_store>();
  auto durable_runtime = std::make_shared<runtime::agent_run_runtime>(store);
  llm_agent_run_options options;
  options.context = { .run_id = "metered-run", .trace_id = "trace-metered" };
  options.runtime = durable_runtime;
  options.pricing = agent::llm::llm_pricing {
    .input_per_million_tokens_usd = 1.0,
    .cached_input_per_million_tokens_usd = 0.5,
    .output_per_million_tokens_usd = 2.0,
    .reasoning_per_million_tokens_usd = 3.0,
  };
  options.callbacks.prepare_model_request = [](llm_request request) {
    request.execution_context.reset();
    return std::optional<llm_request>(std::move(request));
  };
  const auto response = runner.complete("metered read", options);
  require(!response.error_code && response.usage.prompt_tokens == 16 &&
      response.usage.completion_tokens == 6 &&
      response.usage.total_tokens == 22 &&
      response.usage.cached_prompt_tokens == 3 &&
      response.usage.reasoning_tokens == 1,
    "agent usage should aggregate every model call in the tool loop");
  require(response.cost &&
      std::abs(response.cost->total_usd - 0.0000275) < 1e-12,
    "agent cost should be calculated once from aggregate usage");
  require(client.requests.size() == 2 &&
      client.requests.front().execution_context &&
      client.requests.back().execution_context &&
      client.requests.front().execution_context->trace_id == "trace-metered" &&
      client.requests.back().execution_context->run_id == "metered-run",
    "host request preparation must not erase framework execution correlation");
  const auto record = durable_runtime->get("metered-run");
  require(record && record->result["usage"]["total_tokens"] == 22 &&
      std::abs(record->result["cost"]["total_usd"].get<double>() -
        response.cost->total_usd) < 1e-12,
    "durable terminal results should persist aggregate usage and cost");
}

void runner_suspends_and_resumes_exact_tool_call_once() {
  executions = 0;
  approval_then_final_client client;
  auto provider = std::make_shared<tool_provider<guarded_write>>();
  llm_agent_runner runner(client, provider);
  auto store = std::make_shared<runtime::in_memory_agent_run_store>();
  auto durable_runtime = std::make_shared<runtime::agent_run_runtime>(store);
  auto approvals = std::make_shared<manual_approval_service>();

  llm_agent_run_options options;
  options.context = {
    .tenant_id = "tenant-1",
    .user_id = "operator-1",
    .application_id = "isenseguard",
    .conversation_id = "conversation-1",
    .agent_id = "protection-agent",
  };
  options.runtime = durable_runtime;
  options.approval_service = approvals;
  options.pricing = agent::llm::llm_pricing {
    .input_per_million_tokens_usd = 1.0,
    .output_per_million_tokens_usd = 2.0,
  };

  const auto suspended = runner.complete("enable strict mode", options);
  require(suspended.error_code == agent::llm_error_code::approval_required,
    "approval-gated tools should suspend instead of failing the run");
  require(suspended.usage.total_tokens == 10 && suspended.cost,
    "approval suspension should expose the usage and cost already incurred");
  require(executions == 0 && client.requests.size() == 1,
    "no tool in the batch should execute before approval");
  require(approvals->last_request.capabilities.size() == 1 &&
      approvals->last_request.capabilities.front().name ==
        capability::names::filesystem_write,
    "tool contract capabilities should reach the approval boundary");

  const auto run_id = suspended.metadata.at("run_id");
  const auto token = suspended.metadata.at("continuation_token");
  auto waiting = durable_runtime->get(run_id);
  require(waiting &&
      waiting->status == runtime::agent_run_status::waiting_for_approval &&
      waiting->suspension &&
      waiting->suspension->tool_call_id == "write-1",
    "durable runtime should persist the exact pending tool call");

  const auto approved = durable_runtime->resolve_approval(
    run_id,
    waiting->revision,
    token,
    runtime::approval_resolution::approved,
    "approved by operator");
  require(static_cast<bool>(approved),
    "approval resolution should be persisted optimistically");

  const auto claimed_before_crash = durable_runtime->claim_approved_continuation(
    run_id, approved.revision, token);
  require(static_cast<bool>(claimed_before_crash),
    "an approved continuation should be claimable by a worker");
  const auto interrupted = durable_runtime->get(run_id);
  require(interrupted &&
      interrupted->status == runtime::agent_run_status::running &&
      interrupted->active_continuation,
    "a claimed continuation should remain durable until the run terminates");

  options.context = {};
  options.pricing.reset();
  const auto completed = runner.resume(
    run_id, claimed_before_crash.revision, token, options);
  require(!completed.error_code && completed.content == "Update completed.",
    "approved continuation should resume the original model/tool loop");
  require(completed.usage.total_tokens == 16 && completed.cost,
    "durable continuation should preserve accounting configuration across resume");
  require(executions == 1 && client.requests.size() == 2,
    "resumption should execute the approved side effect exactly once");

  const auto final = durable_runtime->get(run_id);
  require(final && final->status == runtime::agent_run_status::completed &&
      !final->active_continuation &&
      final->admitted_tool_results.size() == 1 &&
      final->result.value("content", std::string {}) == "Update completed.",
    "recovered runs should clear continuation state and persist terminal output");
  const auto events = durable_runtime->list_events(run_id);
  require(!events.empty() && events.back().sequence == final->revision,
    "run events should have monotonic replay cursors matching the revision");
  for (std::size_t index = 0; index < events.size(); ++index) {
    require(events[index].sequence == index + 1,
      "run event sequences should be contiguous");
  }
}

void runner_accumulates_approvals_for_a_parallel_tool_batch() {
  executions = 0;
  two_approvals_then_final_client client;
  auto provider = std::make_shared<tool_provider<guarded_write>>();
  llm_agent_runner runner(client, provider);
  auto store = std::make_shared<runtime::in_memory_agent_run_store>();
  auto durable_runtime = std::make_shared<runtime::agent_run_runtime>(store);
  auto approvals = std::make_shared<manual_approval_service>();

  llm_agent_run_options options;
  options.context = { .tenant_id = "tenant-1", .user_id = "operator-1" };
  options.runtime = durable_runtime;
  options.approval_service = approvals;

  auto suspended_a = runner.complete("update both values", options);
  require(suspended_a.error_code == agent::llm_error_code::approval_required &&
      suspended_a.metadata.at("tool_call_id") == "write-a" && executions == 0,
    "the first protected call should suspend the whole batch");
  const auto run_id = suspended_a.metadata.at("run_id");
  const auto token_a = suspended_a.metadata.at("continuation_token");
  auto waiting_a = durable_runtime->get(run_id);
  const auto approved_a = durable_runtime->resolve_approval(
    run_id, waiting_a->revision, token_a, runtime::approval_resolution::approved);

  auto suspended_b = runner.resume(run_id, approved_a.revision, token_a, options);
  require(suspended_b.error_code == agent::llm_error_code::approval_required &&
      suspended_b.metadata.at("tool_call_id") == "write-b" && executions == 0,
    "the second protected call should suspend without re-requesting the first approval");
  const auto token_b = suspended_b.metadata.at("continuation_token");
  auto waiting_b = durable_runtime->get(run_id);
  const auto approved_b = durable_runtime->resolve_approval(
    run_id, waiting_b->revision, token_b, runtime::approval_resolution::approved);

  const auto completed = runner.resume(
    run_id, approved_b.revision, token_b, options);
  require(!completed.error_code && completed.content == "Both updates completed." &&
      executions == 2 && client.requests.size() == 2,
    "accumulated approvals should execute each call once and complete the batch");
  const auto final = durable_runtime->get(run_id);
  require(final && final->status == runtime::agent_run_status::completed &&
      final->admitted_tool_results.size() == 2,
    "the completed batch should durably admit both tool results");
}

#if WUWE_HAS_SQLITE
void sqlite_store_persists_runs_events_and_schema() {
  const auto path = std::filesystem::temp_directory_path() /
    ("wuwe-runtime-" + runtime::agent_run_runtime::make_identifier("test") + ".db");
  std::string run_id;
  {
    auto store = std::make_shared<runtime::sqlite_agent_run_store>(path);
    runtime::agent_run_runtime runtime(store);
    auto record = runtime.start({ .tenant_id = "tenant-sqlite" });
    run_id = record.id;
    const auto running = runtime.transition(
      run_id, record.revision, runtime::agent_run_status::running, "run_started");
    require(static_cast<bool>(running),
      "sqlite runtime should transition a newly created run");
  }
  {
    auto store = std::make_shared<runtime::sqlite_agent_run_store>(path);
    runtime::agent_run_runtime runtime(store);
    const auto record = runtime.get(run_id);
    const auto events = runtime.list_events(run_id);
    require(record && record->status == runtime::agent_run_status::running &&
        events.size() == 2 && events.back().sequence == record->revision,
      "sqlite runtime should recover records and replayable events after reopen");
  }
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.string() + "-wal", ignored);
  std::filesystem::remove(path.string() + "-shm", ignored);
}
#endif

void run(const char* name, void (*test)()) {
  test();
  println("[PASS] {}", name);
}

} // namespace

int main() {
  try {
    run("execution context round trips", execution_context_round_trips_without_runtime_only_state);
    run("run store concurrency and idempotency",
      run_store_enforces_optimistic_concurrency_and_idempotency);
    run("persisted run payload validation",
      persisted_run_payloads_validate_schema_and_error_codes);
    run("tool contract validation", tool_contracts_reject_invalid_descriptors);
    run("runner rejects unregistered and reused tool calls",
      runner_rejects_unregistered_and_reused_tool_calls);
    run("runner enforces tool deadlines",
      runner_enforces_tool_deadlines_without_admitting_late_results);
    run("runner aggregates usage and cost",
      runner_aggregates_usage_cost_and_preserves_execution_context);
    run("runner suspends and resumes tool calls",
      runner_suspends_and_resumes_exact_tool_call_once);
    run("runner accumulates batch approvals",
      runner_accumulates_approvals_for_a_parallel_tool_batch);
#if WUWE_HAS_SQLITE
    run("sqlite run persistence", sqlite_store_persists_runs_events_and_schema);
#endif
  }
  catch (const std::exception& ex) {
    println("[FAIL] {}", ex.what());
    return 1;
  }
  return 0;
}
