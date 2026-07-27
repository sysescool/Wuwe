#include <chrono>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include <wuwe/agent/core/observability.hpp>
#include <wuwe/agent/core/filesystem.hpp>
#include <wuwe/agent/evaluation/security_evaluation.hpp>
#include <wuwe/agent/llm/context_budget.hpp>
#include <wuwe/agent/llm/llm_usage.hpp>
#include <wuwe/agent/llm/resilient_llm_client.hpp>
#include <wuwe/agent/llm/scripted_llm_client.hpp>
#include <wuwe/agent/mcp/mcp_server.hpp>
#include <wuwe/agent/memory/sqlite_memory_store.hpp>
#include <wuwe/agent/knowledge/sqlite_knowledge_index.hpp>
#include <wuwe/agent/runtime/runtime.hpp>

#if WUWE_HAS_SQLITE
#include <sqlite3.h>
#endif

namespace {
using namespace std::chrono_literals;
namespace llm = wuwe::agent::llm;
namespace obs = wuwe::agent::observability;
namespace runtime = wuwe::agent::runtime;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

wuwe::llm_response failure(wuwe::agent::llm_error_code code) {
  return { .content = "scripted failure",
    .error_code = wuwe::agent::make_error_code(code) };
}

class throwing_then_success_client final : public wuwe::llm_client {
public:
  wuwe::llm_response complete(const wuwe::llm_request&) override {
    if (++calls == 1) {
      throw std::runtime_error("transient transport failure");
    }
    return { .content = "recovered from exception" };
  }

  std::atomic<int> calls { 0 };
};

void scripted_client_and_fallback() {
  auto primary = std::make_shared<llm::scripted_llm_client>();
  auto fallback = std::make_shared<llm::scripted_llm_client>();
  primary->push({
    .matches = [](const wuwe::llm_request& request) { return request.model == "requested"; },
    .response = failure(wuwe::agent::llm_error_code::timeout),
  });
  fallback->push({
    .matches = [](const wuwe::llm_request& request) { return request.model == "fallback-model"; },
    .response = { .content = "recovered" },
  });
  llm::resilient_llm_client client({
    { .id = "primary", .client = primary },
    { .id = "fallback", .client = fallback, .model_override = "fallback-model" },
  });
  const auto response = client.complete(wuwe::llm_request { .model = "requested" });
  require(!response.error_code && response.content == "recovered" &&
      response.metadata.at("backend_id") == "fallback" &&
      primary->remaining() == 0 && fallback->remaining() == 0,
    "resilient client should use a deterministic controlled fallback");
}

void scripted_predicates_are_reentrant_and_deterministic() {
  llm::scripted_llm_client client;
  client.push({
    .matches = [&client](const wuwe::llm_request&) {
      return client.remaining() == 1;
    },
    .response = { .content = "matched" },
  });
  const auto response = client.complete({});
  require(response.content == "matched" && client.remaining() == 0,
    "script predicates should be able to inspect client state without deadlock");
}

void resilience_recovers_backend_exceptions_and_preserves_callback_failures() {
  auto throwing = std::make_shared<throwing_then_success_client>();
  llm::resilient_llm_client retrying(
    {{ .id = "throwing", .client = throwing }},
    { .retry = { .max_retries = 1, .initial_backoff = 0ms,
        .max_backoff = 0ms, .jitter_ratio = 0.0 } });
  const auto recovered = retrying.complete({});
  require(recovered.content == "recovered from exception" &&
      throwing->calls == 2,
    "backend exceptions should enter the same bounded retry path as transport errors");

  auto streaming = std::make_shared<llm::scripted_llm_client>();
  streaming->push({
    .response = { .content = "stream completed" },
    .stream_events = { {
      .type = wuwe::llm_stream_event_type::content_delta,
      .content_delta = "visible",
    } },
  });
  streaming->push({ .response = { .content = "must not retry" } });
  llm::resilient_llm_client protected_callbacks(
    {{ .id = "streaming", .client = streaming }},
    { .retry = { .max_retries = 1, .initial_backoff = 0ms,
        .max_backoff = 0ms, .jitter_ratio = 0.0 } });
  bool propagated = false;
  try {
    (void)protected_callbacks.complete_stream({}, {
      .on_event = [](const wuwe::llm_stream_event&) {
        throw std::runtime_error("consumer callback failed");
      },
    });
  }
  catch (const std::runtime_error& error) {
    propagated = std::string(error.what()) == "consumer callback failed";
  }
  require(propagated && streaming->remaining() == 1,
    "consumer callback exceptions must propagate without poisoning provider health or retrying");
}

void resilience_aggregates_usage_across_retries_and_fallbacks() {
  auto primary = std::make_shared<llm::scripted_llm_client>();
  auto fallback = std::make_shared<llm::scripted_llm_client>();
  auto failed = failure(wuwe::agent::llm_error_code::timeout);
  failed.usage = { .prompt_tokens = 10, .completion_tokens = 2,
    .total_tokens = 12, .cached_prompt_tokens = 3,
    .reasoning_tokens = 1 };
  primary->push({ .response = failed });
  fallback->push({ .response = {
    .content = "fallback",
    .usage = { .prompt_tokens = 5, .completion_tokens = 4,
      .total_tokens = 9, .cached_prompt_tokens = 1,
      .reasoning_tokens = 2 },
  } });
  llm::resilient_llm_client client({
    { .id = "primary", .client = primary },
    { .id = "fallback", .client = fallback },
  });
  const auto response = client.complete({});
  require(response.content == "fallback" &&
      response.usage.prompt_tokens == 15 &&
      response.usage.completion_tokens == 6 &&
      response.usage.total_tokens == 21 &&
      response.usage.cached_prompt_tokens == 4 &&
      response.usage.reasoning_tokens == 3,
    "resilience accounting should include failed attempts and successful fallbacks");
}

void resilience_boundaries() {
  {
    auto backend = std::make_shared<llm::scripted_llm_client>();
    auto retryable = failure(wuwe::agent::llm_error_code::rate_limited);
    retryable.metadata["retry_after_ms"] = "0";
    backend->push({ .response = retryable });
    backend->push({ .response = { .content = "retried" } });
    llm::resilient_llm_client client(
      {{ .id = "retry", .client = backend }},
      { .retry = { .max_retries = 1, .initial_backoff = 0ms,
          .max_backoff = 0ms, .max_server_delay = 0ms, .jitter_ratio = 0.0 } });
    require(client.complete({}).content == "retried",
      "rate-limited requests should honor the retry policy");
  }
  {
    auto backend = std::make_shared<llm::scripted_llm_client>();
    backend->push({ .response = failure(wuwe::agent::llm_error_code::timeout) });
    backend->push({ .response = { .content = "half-open recovered" } });
    llm::resilient_llm_client client(
      {{ .id = "circuit", .client = backend }},
      { .circuit_breaker = { .failure_threshold = 1, .open_duration = 2ms } });
    require(client.complete({}).error_code == wuwe::agent::llm_error_code::timeout,
      "first backend failure should be preserved");
    require(client.complete({}).error_code == wuwe::agent::llm_error_code::circuit_open,
      "open circuit should reject without invoking the backend");
    std::this_thread::sleep_for(4ms);
    require(client.complete({}).content == "half-open recovered",
      "a successful half-open probe should close the circuit");
  }
  {
    auto backend = std::make_shared<llm::scripted_llm_client>();
    backend->push({ .response = { .content = "first" } });
    backend->push({ .response = { .content = "unused" } });
    llm::resilient_llm_client client(
      {{ .id = "limited", .client = backend }},
      { .rate_limit = { .max_requests = 1, .window = 1s, .max_wait = 0ms } });
    require(client.complete({}).content == "first", "first request should enter the window");
    require(client.complete({}).error_code ==
        wuwe::agent::llm_error_code::rate_limit_wait_exceeded &&
        backend->remaining() == 1,
      "local rate limiter should reject before invoking the backend");
  }
  {
    auto backend = std::make_shared<llm::scripted_llm_client>();
    backend->push({
      .response = failure(wuwe::agent::llm_error_code::timeout),
      .stream_events = {{ .type = wuwe::llm_stream_event_type::content_delta,
        .content_delta = "partial" }},
    });
    backend->push({ .response = { .content = "must not retry" } });
    llm::resilient_llm_client client(
      {{ .id = "stream", .client = backend }},
      { .retry = { .max_retries = 1, .initial_backoff = 0ms,
          .max_backoff = 0ms, .jitter_ratio = 0.0 } });
    int errors = 0;
    const auto response = client.complete_stream({}, {
      .on_event = [&](const wuwe::llm_stream_event& event) {
        if (event.type == wuwe::llm_stream_event_type::error) ++errors;
      },
    });
    require(response.error_code == wuwe::agent::llm_error_code::timeout &&
        errors == 1 && backend->remaining() == 1,
      "streaming must never retry after user-visible output");
  }
  {
    auto backend = std::make_shared<llm::scripted_llm_client>();
    backend->push({ .response = failure(wuwe::agent::llm_error_code::timeout) });
    backend->push({ .response = { .content = "recovered after jitter callback" } });
    llm::resilient_llm_client client(
      {{ .id = "throwing-jitter", .client = backend }},
      { .retry = { .max_retries = 1, .initial_backoff = 0ms,
          .max_backoff = 0ms, .jitter_ratio = 0.5 },
        .random_unit = []() -> double {
          throw std::runtime_error("random source unavailable");
        } });
    require(client.complete({}).content == "recovered after jitter callback",
      "optional jitter callbacks must not terminate the retry path");
  }
  {
    auto backend = std::make_shared<llm::scripted_llm_client>();
    backend->push({ .response = failure(wuwe::agent::llm_error_code::timeout) });
    backend->push({ .response = { .content = "recovered after non-finite jitter" } });
    llm::resilient_llm_client client(
      {{ .id = "nan-jitter", .client = backend }},
      { .retry = { .max_retries = 1, .initial_backoff = 0ms,
          .max_backoff = 0ms, .jitter_ratio = 0.5 },
        .random_unit = [] {
          return (std::numeric_limits<double>::quiet_NaN)();
        } });
    require(client.complete({}).content == "recovered after non-finite jitter",
      "non-finite jitter values must fall back to a deterministic safe value");
  }
}

void detailed_usage_and_cost() {
  const wuwe::llm_usage usage { .prompt_tokens = 1000,
    .completion_tokens = 500, .total_tokens = 1500,
    .cached_prompt_tokens = 250, .reasoning_tokens = 100 };
  const auto cost = llm::calculate_llm_cost(usage, {
    .input_per_million_tokens_usd = 4.0,
    .cached_input_per_million_tokens_usd = 1.0,
    .output_per_million_tokens_usd = 8.0,
    .reasoning_per_million_tokens_usd = 10.0,
  });
  require(cost && std::abs(cost->total_usd - 0.00745) < 1e-12,
    "cost accounting should price cached and reasoning tokens separately");
  auto invalid = usage;
  invalid.cached_prompt_tokens = 1001;
  require(!llm::calculate_llm_cost(invalid, {}),
    "cost accounting should reject inconsistent provider usage");
  const auto saturated = llm::calculate_llm_cost(
    { .prompt_tokens = (std::numeric_limits<int>::max)(),
      .total_tokens = (std::numeric_limits<int>::max)() },
    { .input_per_million_tokens_usd =
        (std::numeric_limits<double>::max)() });
  require(saturated && std::isfinite(saturated->input_usd) &&
      std::isfinite(saturated->total_usd),
    "cost accounting should saturate instead of emitting non-finite JSON values");

  wuwe::llm_usage accumulated {
    .prompt_tokens = 2,
    .completion_tokens = 3,
  };
  llm::accumulate_llm_usage(accumulated, {
    .prompt_tokens = 7,
    .completion_tokens = 11,
    .total_tokens = 1,
  });
  require(accumulated.prompt_tokens == 9 &&
      accumulated.completion_tokens == 14 &&
      accumulated.total_tokens == 23,
    "usage aggregation must derive a conservative total when providers omit or underreport it");
}

void context_budget_accounting_saturates() {
  class maximum_estimator final : public llm::context_token_estimator {
  public:
    std::size_t estimate_text(std::string_view) const override {
      return (std::numeric_limits<std::size_t>::max)();
    }
    std::string truncate_text(
      std::string_view,
      std::size_t,
      bool) const override {
      return {};
    }
  };

  llm::context_budget_manager manager(
    std::make_shared<maximum_estimator>());
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "oversized" });
  const auto result = manager.fit(std::move(request), {
    .context_window_tokens = 1024,
    .reserved_output_tokens = 16,
    .overflow = wuwe::llm_context_overflow_policy::reject,
  });
  require(!result &&
      result.report.before.input_total() ==
        (std::numeric_limits<std::size_t>::max)(),
    "context accounting must saturate instead of wrapping a custom estimator");
}

void observability_and_replay() {
  obs::agent_event event { .module = "runtime", .name = "tool_completed",
    .trace_id = "trace-1", .subject_id = "run-1", .run_id = "run-1",
    .sequence = 7, .request_id = "request-1", .step_id = "step-1",
    .tool_call_id = "call-1", .data = {{ "ok", true }} };
  const auto restored = obs::agent_event_from_json(obs::agent_event_to_json(event));
  require(restored.run_id == "run-1" && restored.sequence == 7 &&
      restored.tool_call_id == "call-1" && restored.data["ok"].get<bool>(),
    "agent event correlation fields should round-trip");

  auto memory = std::make_shared<obs::in_memory_event_sink>();
  obs::async_event_sink async(memory, { .capacity = 8 });
  async.publish(event);
  async.flush();
  const auto stats = async.stats();
  require(stats.published == 1 && stats.delivered == 1 &&
      stats.failures == 0 && memory->events().size() == 1,
    "async event sink should drain successfully");

  bool rejected_destination = false;
  try {
    obs::async_event_sink invalid(nullptr);
  }
  catch (const std::invalid_argument&) {
    rejected_destination = true;
  }
  bool rejected_capacity = false;
  try {
    obs::async_event_sink invalid(memory, { .capacity = 0 });
  }
  catch (const std::invalid_argument&) {
    rejected_capacity = true;
  }
  require(rejected_destination && rejected_capacity,
    "async event sink must validate construction before starting its worker");

  auto store = std::make_shared<runtime::in_memory_agent_run_store>();
  runtime::agent_run_runtime run(store);
  auto record = run.start({ .run_id = "replay-run", .trace_id = "replay-trace" });
  const auto transitioned = run.transition(record.id, record.revision,
    runtime::agent_run_status::running, "run_started");
  obs::in_memory_event_sink replay;
  require(runtime::replay_run_events(*store, record.id, replay, 1, "replay-trace") == 1 &&
      replay.events().front().sequence == transitioned.revision,
    "runtime events should replay after an exclusive sequence cursor");
}

void security_evaluation() {
  wuwe::agent::evaluation::security_invariant_evaluator evaluator;
  const auto safe = wuwe::agent::evaluation::make_security_evaluation_case(
    "safe", wuwe::agent::evaluation::security_scenario::prompt_injection, {});
  require(evaluator.evaluate(safe).passed, "safe observation should pass");
  const auto unsafe = wuwe::agent::evaluation::make_security_evaluation_case(
    "unsafe", wuwe::agent::evaluation::security_scenario::cross_tenant_access,
    { .cross_tenant_data_exposed = true, .evidence = { "tenant-b record" } });
  const auto result = evaluator.evaluate(unsafe);
  require(!result.passed && result.evidence.size() == 2,
    "security evaluation should retain invariant and diagnostic evidence");
}

void mcp_protocol_negotiation() {
  wuwe::agent::mcp::mcp_server server;
  const auto legacy = server.handle_message(nlohmann::json {
    { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "initialize" },
    { "params", {{ "protocolVersion", "2024-11-05" }} },
  }.dump());
  require(legacy && nlohmann::json::parse(*legacy)["result"]["protocolVersion"] ==
      "2024-11-05" && server.negotiated_protocol_version() == "2024-11-05",
    "MCP server should negotiate a supported legacy version");
  wuwe::agent::mcp::mcp_server incompatible;
  const auto rejected = incompatible.handle_message(nlohmann::json {
    { "jsonrpc", "2.0" }, { "id", 2 }, { "method", "initialize" },
    { "params", {{ "protocolVersion", "2030-01-01" }} },
  }.dump());
  const auto response = nlohmann::json::parse(*rejected);
  require(response.contains("error") &&
      response["error"]["data"]["supportedVersions"].size() == 2,
    "unknown MCP versions should return compatibility metadata");
}

#if WUWE_HAS_SQLITE
void sqlite_migrations() {
  const auto base = std::filesystem::temp_directory_path() /
    runtime::agent_run_runtime::make_identifier("wuwe-p1-migration");
  const auto legacy_path = base.string() + "-legacy.db";
  sqlite3* db {};
  require(sqlite3_open(legacy_path.c_str(), &db) == SQLITE_OK, "open legacy fixture");
  sqlite3_exec(db, "CREATE TABLE agent_runs (id TEXT PRIMARY KEY, revision INTEGER NOT NULL, document_json TEXT NOT NULL);", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "CREATE TABLE agent_run_events (run_id TEXT NOT NULL, sequence INTEGER NOT NULL, document_json TEXT NOT NULL, PRIMARY KEY(run_id, sequence));", nullptr, nullptr, nullptr);
  sqlite3_close(db);
  {
    runtime::sqlite_agent_run_store store(legacy_path);
    require(store.schema_version() == runtime::sqlite_agent_run_store::latest_schema_version,
      "unversioned legacy schema should migrate transactionally");
  }
  const auto future_path = base.string() + "-future.db";
  require(sqlite3_open(future_path.c_str(), &db) == SQLITE_OK, "open future fixture");
  sqlite3_exec(db, "CREATE TABLE agent_runtime_schema (component TEXT PRIMARY KEY, version INTEGER NOT NULL);", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "INSERT INTO agent_runtime_schema VALUES ('run_store', 99);", nullptr, nullptr, nullptr);
  sqlite3_close(db);
  bool rejected = false;
  try { runtime::sqlite_agent_run_store store(future_path); }
  catch (const std::runtime_error&) { rejected = true; }
  require(rejected, "future database schema should be rejected explicitly");

  const auto memory_path = base.string() + "-memory.db";
  {
    wuwe::agent::memory::sqlite_memory_store store(memory_path);
    require(store.schema_version() ==
          wuwe::agent::memory::sqlite_memory_store::latest_schema_version &&
        store.capabilities().schema_migrations,
      "SQLite memory store should publish its active migration version");
    store.add({
      .id = "memory-1",
      .kind = wuwe::agent::memory::memory_kind::working,
      .content = "preserved",
    });
    store.add({
      .id = "mem-7",
      .kind = wuwe::agent::memory::memory_kind::working,
      .content = "sequence migration anchor",
    });
  }
  require(sqlite3_open(memory_path.c_str(), &db) == SQLITE_OK,
    "open memory migration fixture");
  sqlite3_exec(db,
    "DELETE FROM wuwe_storage_schema WHERE component = 'memory_store'",
    nullptr, nullptr, nullptr);
  sqlite3_close(db);
  {
    wuwe::agent::memory::sqlite_memory_store store(memory_path);
    const auto restored = store.get("memory-1", {});
    require(store.schema_version() ==
          wuwe::agent::memory::sqlite_memory_store::latest_schema_version && restored &&
        restored->content == "preserved",
      "unversioned memory schema should migrate without losing records");
  }
  require(sqlite3_open(memory_path.c_str(), &db) == SQLITE_OK,
    "open memory v1 migration fixture");
  sqlite3_exec(db, "DROP TABLE wuwe_memory_sequence", nullptr, nullptr, nullptr);
  sqlite3_exec(db,
    "UPDATE wuwe_storage_schema SET version = 1 "
    "WHERE component = 'memory_store'", nullptr, nullptr, nullptr);
  sqlite3_close(db);
  {
    wuwe::agent::memory::sqlite_memory_store store(memory_path);
    const auto generated = store.add({
      .kind = wuwe::agent::memory::memory_kind::working,
      .content = "after sequence migration",
    });
    require(generated.id == "mem-8",
      "memory v1-to-v2 migration should initialize its sequence from existing ids");
  }
  const auto coordinated_memory_path = base.string() + "-memory-coordination.db";
  {
    wuwe::agent::memory::sqlite_memory_store first(coordinated_memory_path);
    wuwe::agent::memory::sqlite_memory_store second(coordinated_memory_path);
    const auto first_record = first.add({
      .kind = wuwe::agent::memory::memory_kind::working,
      .content = "first",
    });
    const auto second_record = second.add({
      .id = "mem-100",
      .kind = wuwe::agent::memory::memory_kind::working,
      .content = "second",
    });
    const auto third_record = first.add({
      .kind = wuwe::agent::memory::memory_kind::working,
      .content = "third",
    });
    require(first_record.id == "mem-1" && second_record.id == "mem-100" &&
        third_record.id == "mem-101" && first.get(second_record.id, {}).has_value(),
      "SQLite memory identifiers should coordinate across connections and explicit ids");
  }
  require(sqlite3_open(memory_path.c_str(), &db) == SQLITE_OK,
    "open future memory fixture");
  sqlite3_exec(db,
    "UPDATE wuwe_storage_schema SET version = 99 "
    "WHERE component = 'memory_store'", nullptr, nullptr, nullptr);
  sqlite3_close(db);
  rejected = false;
  try { wuwe::agent::memory::sqlite_memory_store store(memory_path); }
  catch (const std::runtime_error&) { rejected = true; }
  require(rejected, "future memory schema should be rejected explicitly");

  const auto knowledge_path = base.string() + "-knowledge.db";
  {
    wuwe::agent::knowledge::sqlite_knowledge_index index(knowledge_path);
    require(index.schema_version() ==
          wuwe::agent::knowledge::sqlite_knowledge_index::latest_schema_version &&
        index.capabilities().schema_migrations,
      "SQLite knowledge index should publish its active migration version");
    index.upsert({
      .id = "chunk-1",
      .document_id = "document-1",
      .content = "preserved",
    }, { 1.0F, 0.0F });
  }
  require(sqlite3_open(knowledge_path.c_str(), &db) == SQLITE_OK,
    "open knowledge migration fixture");
  sqlite3_exec(db,
    "DELETE FROM wuwe_storage_schema WHERE component = 'knowledge_index'",
    nullptr, nullptr, nullptr);
  sqlite3_close(db);
  {
    wuwe::agent::knowledge::sqlite_knowledge_index index(knowledge_path);
    require(index.schema_version() ==
          wuwe::agent::knowledge::sqlite_knowledge_index::latest_schema_version &&
        index.search({ .text = "preserved", .limit = 1 },
          { 1.0F, 0.0F }).size() == 1,
      "unversioned knowledge schema should migrate without losing entries");
  }
  require(sqlite3_open(knowledge_path.c_str(), &db) == SQLITE_OK,
    "open future knowledge fixture");
  sqlite3_exec(db,
    "UPDATE wuwe_storage_schema SET version = 99 "
    "WHERE component = 'knowledge_index'", nullptr, nullptr, nullptr);
  sqlite3_close(db);
  rejected = false;
  try { wuwe::agent::knowledge::sqlite_knowledge_index index(knowledge_path); }
  catch (const std::runtime_error&) { rejected = true; }
  require(rejected, "future knowledge schema should be rejected explicitly");

  const auto malformed_run_path = base.string() + "-malformed-run.db";
  require(sqlite3_open(malformed_run_path.c_str(), &db) == SQLITE_OK,
    "open malformed run fixture");
  sqlite3_exec(db, "CREATE TABLE agent_runtime_schema (component TEXT PRIMARY KEY, version INTEGER NOT NULL);", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "INSERT INTO agent_runtime_schema VALUES ('run_store', 1);", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "CREATE TABLE agent_runs (id TEXT PRIMARY KEY, revision INTEGER NOT NULL);", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "CREATE TABLE agent_run_events (run_id TEXT NOT NULL, sequence INTEGER NOT NULL, document_json TEXT NOT NULL, PRIMARY KEY(run_id, sequence));", nullptr, nullptr, nullptr);
  sqlite3_close(db);
  rejected = false;
  try { runtime::sqlite_agent_run_store store(malformed_run_path); }
  catch (const std::runtime_error&) { rejected = true; }
  require(rejected, "run store should reject a versioned table with an incompatible shape");

  const auto malformed_memory_path = base.string() + "-malformed-memory.db";
  require(sqlite3_open(malformed_memory_path.c_str(), &db) == SQLITE_OK,
    "open malformed memory fixture");
  sqlite3_exec(db, "CREATE TABLE wuwe_storage_schema (component TEXT PRIMARY KEY, version INTEGER NOT NULL);", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "INSERT INTO wuwe_storage_schema VALUES ('memory_store', 2);", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "CREATE TABLE memory_records (id TEXT PRIMARY KEY);", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "CREATE TABLE wuwe_memory_sequence (name TEXT PRIMARY KEY, last_value INTEGER NOT NULL);", nullptr, nullptr, nullptr);
  sqlite3_close(db);
  rejected = false;
  try { wuwe::agent::memory::sqlite_memory_store store(malformed_memory_path); }
  catch (const std::runtime_error&) { rejected = true; }
  require(rejected, "memory store should reject a versioned table with an incompatible shape");

  const auto malformed_knowledge_path = base.string() + "-malformed-knowledge.db";
  require(sqlite3_open(malformed_knowledge_path.c_str(), &db) == SQLITE_OK,
    "open malformed knowledge fixture");
  sqlite3_exec(db, "CREATE TABLE wuwe_storage_schema (component TEXT PRIMARY KEY, version INTEGER NOT NULL);", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "INSERT INTO wuwe_storage_schema VALUES ('knowledge_index', 1);", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "CREATE TABLE knowledge_index (chunk_id TEXT PRIMARY KEY, document_id TEXT NOT NULL);", nullptr, nullptr, nullptr);
  sqlite3_close(db);
  rejected = false;
  try { wuwe::agent::knowledge::sqlite_knowledge_index index(malformed_knowledge_path); }
  catch (const std::runtime_error&) { rejected = true; }
  require(rejected,
    "knowledge index should reject a versioned table with an incompatible shape");

  std::error_code ignored;
  std::filesystem::remove(legacy_path, ignored);
  std::filesystem::remove(future_path, ignored);
  std::filesystem::remove(memory_path, ignored);
  std::filesystem::remove(coordinated_memory_path, ignored);
  std::filesystem::remove(knowledge_path, ignored);
  std::filesystem::remove(malformed_run_path, ignored);
  std::filesystem::remove(malformed_memory_path, ignored);
  std::filesystem::remove(malformed_knowledge_path, ignored);
}

void sqlite_unicode_paths() {
  const auto directory = std::filesystem::temp_directory_path() /
    runtime::agent_run_runtime::make_identifier("wuwe-unicode") /
    std::filesystem::path(L"数据存储");
  const auto memory_path = directory / std::filesystem::path(L"记忆.db");
  const auto knowledge_path = directory / std::filesystem::path(L"知识.db");

  {
    wuwe::agent::memory::sqlite_memory_store store(memory_path);
    const auto record = store.add({
      .kind = wuwe::agent::memory::memory_kind::working,
      .content = "unicode path memory",
    });
    require(store.get(record.id, {}).has_value(),
      "SQLite memory store should support Unicode filesystem paths");
  }
  {
    wuwe::agent::knowledge::sqlite_knowledge_index index(knowledge_path);
    index.upsert({
      .id = "unicode-chunk",
      .document_id = "unicode-document",
      .content = "unicode path knowledge",
    }, { 1.0F, 0.0F });
    require(index.search({ .text = "knowledge", .limit = 1 },
        { 1.0F, 0.0F }).size() == 1,
      "SQLite knowledge index should support Unicode filesystem paths");
  }

  std::error_code ignored;
  std::filesystem::remove_all(directory.parent_path(), ignored);
}
#endif

void run(const char* name, void (*test)()) {
  test();
  std::cout << "[PASS] " << name << '\n';
}
} // namespace

int main() {
  try {
    run("scripted client and fallback", scripted_client_and_fallback);
    run("scripted predicate reentrancy", scripted_predicates_are_reentrant_and_deterministic);
    run("resilience exception boundaries",
      resilience_recovers_backend_exceptions_and_preserves_callback_failures);
    run("resilience usage aggregation",
      resilience_aggregates_usage_across_retries_and_fallbacks);
    run("resilience boundaries", resilience_boundaries);
    run("detailed usage and cost", detailed_usage_and_cost);
    run("context budget saturation", context_budget_accounting_saturates);
    run("observability and replay", observability_and_replay);
    run("security evaluation", security_evaluation);
    run("MCP protocol negotiation", mcp_protocol_negotiation);
#if WUWE_HAS_SQLITE
    run("SQLite migrations", sqlite_migrations);
    run("SQLite Unicode paths", sqlite_unicode_paths);
#endif
  }
  catch (const std::exception& error) {
    std::cerr << "[FAIL] " << error.what() << '\n';
    return 1;
  }
  return 0;
}
