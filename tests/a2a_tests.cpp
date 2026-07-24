#include <map>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>

#include <wuwe/agent/a2a/a2a.hpp>
#include <wuwe/agent/a2a/multi_agent_adapter.hpp>
#include <wuwe/common/print.h>

namespace {
namespace a2a = wuwe::agent::a2a;
namespace ma = wuwe::agent::multi_agent;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

a2a::agent_card test_card() {
  return {
    .name = "review-agent",
    .description = "Reviews text",
    .url = "https://agents.example/a2a",
    .version = "1.0.0",
    .skills = { {
      .id = "review",
      .name = "Review",
      .tags = { "quality" },
      .input_modes = { "text/plain" },
      .output_modes = { "text/plain" },
    } },
  };
}

class conservative_transport final : public a2a::transport {
public:
  a2a::rpc_result invoke(
    std::string,
    nlohmann::json,
    std::stop_token) override {
    return { .failure = a2a::error {
      .code = a2a::error_code::transport_error,
      .message = "not invoked",
    } };
  }

  a2a::result<a2a::agent_card> discover(std::stop_token) override {
    return { .value = test_card() };
  }
};

class memory_handler final : public a2a::task_handler {
public:
  a2a::result<a2a::task> send(
    const a2a::send_message_params& params,
    std::stop_token) override {
    a2a::task value {
      .id = params.value.task_id,
      .context_id = params.value.context_id,
      .status = { .state = a2a::task_state::completed },
      .artifacts = { {
        .artifact_id = params.value.task_id + ":result",
        .name = "result",
        .parts = { a2a::part::text_part("remote answer") },
        .metadata = { { "source", "remote" } },
      } },
      .history = { params.value },
      .metadata = params.metadata,
    };
    tasks[value.id] = value;
    return { .value = std::move(value) };
  }

  a2a::result<a2a::task> get(const a2a::task_query_params& params) override {
    const auto found = tasks.find(params.id);
    if (found == tasks.end()) {
      return { .failure = a2a::error {
        .code = a2a::error_code::task_not_found,
        .message = "missing",
      } };
    }
    return { .value = found->second };
  }

  a2a::result<a2a::task> cancel(const a2a::task_id_params&) override {
    return { .failure = a2a::error {
      .code = a2a::error_code::task_not_cancelable,
      .message = "complete",
    } };
  }

  std::map<std::string, a2a::task> tasks;
};

void codecs_and_in_process_protocol_work() {
  const auto card = a2a::agent_card_from_json(a2a::to_json(test_card()));
  require(card.name == "review-agent" && card.skills.front().id == "review",
    "Agent Card codec preserves discovery and skill fields");

  auto service = std::make_shared<a2a::service>(test_card(), std::make_shared<memory_handler>());
  a2a::client client(std::make_shared<a2a::in_process_transport>(service));
  require(static_cast<bool>(client.discover()),
    "in-process transport exposes Agent Card discovery");
  const auto sent = client.send({
    .value = {
      .message_id = "message-1",
      .parts = { a2a::part::text_part("review") },
      .task_id = "task-1",
      .context_id = "context-1",
    },
  });
  require(sent && sent.value->artifacts.front().parts.front().text == "remote answer",
    "message/send returns typed Task and Artifact values");
  require(static_cast<bool>(client.get({ .id = "task-1" })),
    "tasks/get retrieves task state");
  const auto missing = client.get({ .id = "missing" });
  require(!missing && missing.failure->code == a2a::error_code::task_not_found,
    "A2A error codes survive client and transport boundaries");
}

class capture_http_client final : public wuwe::http_client {
public:
  wuwe::http_response send(const wuwe::http_request& request) override {
    requests.push_back(request);
    if (request.method == "GET") {
      if (discovery_stop_source) discovery_stop_source->request_stop();
      return { .status_code = 200, .body = a2a::to_json(test_card()).dump() };
    }
    const auto rpc = nlohmann::json::parse(request.body);
    auto body = nlohmann::json {
      { "jsonrpc", "2.0" },
      { "id", wrong_response_id
                  ? nlohmann::json(rpc.at("id").get<std::uint64_t>() + 1)
                  : rpc.at("id") },
      { "result", a2a::to_json(a2a::task {
        .id = "task-http",
        .status = { .state = a2a::task_state::completed },
      }) },
    };
    if (both_result_and_error) {
      body["error"] = {
        { "code", static_cast<int>(a2a::error_code::internal_error) },
        { "message", "ambiguous" },
      };
    }
    return {
      .status_code = 200,
      .body = body.dump(),
    };
  }
  std::vector<wuwe::http_request> requests;
  bool wrong_response_id { false };
  bool both_result_and_error { false };
  std::stop_source* discovery_stop_source {};
};

void http_client_and_service_adapters_work() {
  auto http = std::make_shared<capture_http_client>();
  auto transport = std::make_shared<a2a::http_client_transport>(
    a2a::http_client_transport_options {
      .endpoint = "https://agents.example/a2a",
      .headers = { { "Authorization", "Bearer token" } },
    }, http);
  a2a::client client(transport);
  require(static_cast<bool>(client.discover()), "HTTP transport discovers Agent Cards");
  require(http->requests.front().url ==
      "https://agents.example/.well-known/agent-card.json",
    "HTTP transport derives the well-known Agent Card URL from the endpoint origin");
  require(static_cast<bool>(client.send({ .value = {
    .message_id = "message-http",
    .parts = { a2a::part::text_part("hello") },
    .task_id = "task-http",
  } })), "HTTP transport invokes message/send");
  const auto rpc = nlohmann::json::parse(http->requests.back().body);
  require(rpc.at("method") == "message/send" &&
      rpc.at("params").at("message").at("kind") == "message",
    "HTTP transport emits A2A JSON-RPC payloads");

  auto service = std::make_shared<a2a::service>(test_card(), std::make_shared<memory_handler>());
  a2a::http_service_adapter adapter(service);
  const auto card_response = adapter.handle({
    .method = "GET",
    .path = "/.well-known/agent-card.json",
  });
  require(card_response.status_code == 200 &&
      nlohmann::json::parse(card_response.body).at("name") == "review-agent",
    "HTTP service adapter serves the well-known Agent Card");

  http->wrong_response_id = true;
  const auto mismatched = client.send({ .value = {
    .message_id = "mismatch-message",
    .parts = { a2a::part::text_part("hello") },
    .task_id = "mismatch-task",
  } });
  require(!mismatched &&
      mismatched.failure->code == a2a::error_code::invalid_agent_response,
    "HTTP transport rejects mismatched JSON-RPC response IDs");

  http->wrong_response_id = false;
  http->both_result_and_error = true;
  const auto ambiguous = client.send({ .value = {
    .message_id = "ambiguous-message",
    .parts = { a2a::part::text_part("hello") },
    .task_id = "ambiguous-task",
  } });
  require(!ambiguous && ambiguous.failure->code == a2a::error_code::invalid_agent_response,
    "HTTP transport rejects JSON-RPC responses containing result and error");
  http->both_result_and_error = false;

  std::stop_source discovery_stop_source;
  http->discovery_stop_source = &discovery_stop_source;
  const auto cancelled_discovery = client.discover(discovery_stop_source.get_token());
  require(!cancelled_discovery &&
      cancelled_discovery.failure->code == a2a::error_code::transport_error,
    "HTTP discovery observes cancellation requested during the transport call");
}

void service_rejects_unsupported_advertised_capabilities() {
  auto card = test_card();
  card.capabilities.streaming = true;
  bool rejected = false;
  try {
    a2a::service invalid(card, std::make_shared<memory_handler>());
  }
  catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected,
    "synchronous services cannot advertise unsupported streaming capabilities");
}

void jsonrpc_notifications_and_invalid_requests_are_handled() {
  auto handler = std::make_shared<memory_handler>();
  auto service = std::make_shared<a2a::service>(test_card(), handler);
  const auto notification = service->handle_jsonrpc({
    { "jsonrpc", "2.0" },
    { "method", "message/send" },
    { "params", {
      { "message", a2a::to_json(a2a::message {
        .message_id = "notification-message",
        .parts = { a2a::part::text_part("notify") },
        .task_id = "notification-task",
      }) },
    } },
  });
  require(notification.is_null() && handler->tasks.contains("notification-task"),
    "JSON-RPC notifications execute without producing a response");

  const auto malformed = service->handle_jsonrpc({
    { "jsonrpc", "2.0" },
    { "id", 1 },
    { "method", "tasks/get" },
    { "params", nlohmann::json::array() },
  });
  require(malformed.at("error").at("code") ==
      static_cast<int>(a2a::error_code::invalid_params),
    "malformed method params map to a stable invalid-params error");
  const auto invalid_id = service->handle_jsonrpc({
    { "jsonrpc", "2.0" },
    { "id", 1.5 },
    { "method", "tasks/get" },
    { "params", { { "id", "notification-task" } } },
  });
  require(invalid_id.at("id").is_null() && invalid_id.at("error").at("code") ==
      static_cast<int>(a2a::error_code::invalid_request),
    "invalid JSON-RPC id types are rejected with a null response id");

  a2a::client client(std::make_shared<a2a::in_process_transport>(service));
  const auto unsupported_mode = client.send({
    .value = {
      .message_id = "unsupported-mode-message",
      .parts = { a2a::part::text_part("answer") },
      .task_id = "unsupported-mode-task",
    },
    .configuration = { .accepted_output_modes = { "application/xml" } },
  });
  require(!unsupported_mode && unsupported_mode.failure->code ==
      a2a::error_code::content_type_not_supported,
    "message/send rejects output-mode negotiations with no supported intersection");

  a2a::http_service_adapter adapter(service);
  const auto http_notification = adapter.handle({
    .method = "POST",
    .path = "/a2a",
    .body = nlohmann::json {
      { "jsonrpc", "2.0" },
      { "method", "message/send" },
      { "params", {
        { "message", a2a::to_json(a2a::message {
          .message_id = "http-notification-message",
          .parts = { a2a::part::text_part("notify") },
          .task_id = "http-notification-task",
        }) },
      } },
    }.dump(),
  });
  require(http_notification.status_code == 204 && http_notification.body.empty(),
    "HTTP notifications return 204 with no JSON-RPC body");
}

void remote_and_local_team_adapters_bridge_a2a() {
  auto remote_service = std::make_shared<a2a::service>(
    test_card(), std::make_shared<memory_handler>());
  auto remote_client = std::make_shared<a2a::client>(
    std::make_shared<a2a::in_process_transport>(remote_service));
  auto registry = std::make_shared<ma::agent_registry>();
  registry->add(
    a2a::agent_descriptor_from_card(test_card(), "remote-reviewer"),
    std::make_shared<a2a::remote_agent_executor>(remote_client));
  ma::team_runtime runtime({ .registry = registry });
  const auto remote = runtime.run({
    .id = "remote-task",
    .session_id = "remote-context",
    .input = "review",
    .preferred_agent = "remote-reviewer",
    .metadata = { { "trace_id", "remote-trace" } },
  });
  require(remote && remote.output == "remote answer" &&
      remote.metadata.at("trace_id") == "remote-trace" &&
      remote.artifacts.front().metadata.at("source") == "remote",
    "remote A2A agents preserve Task and Artifact metadata locally");

  auto local_registry = std::make_shared<ma::agent_registry>();
  std::map<std::string, std::string> observed_metadata;
  local_registry->add({ .id = "local", .name = "local" },
    std::make_shared<ma::function_agent_executor>(
      [&](const ma::agent_task_request& request, const ma::agent_execution_context&) {
        observed_metadata = request.metadata;
        return ma::agent_task_result { .output = "local:" + request.input };
      }));
  auto local_runtime = std::make_shared<ma::team_runtime>(ma::team_runtime_options {
    .registry = local_registry,
  });
  auto handler = std::make_shared<a2a::team_task_handler>(a2a::team_task_handler_options {
    .runtime = local_runtime,
    .preferred_agent = "local",
  });
  a2a::client local_client(std::make_shared<a2a::in_process_transport>(
    std::make_shared<a2a::service>(test_card(), handler)));
  const auto exposed = local_client.send({ .value = {
    .message_id = "local-message",
    .parts = { a2a::part::text_part("input") },
    .task_id = "local-task",
    .context_id = "local-context",
    .metadata = { { "trace_id", "message-trace" }, { "attempt", 7 } },
  }, .metadata = { { "trace_id", "request-trace" }, { "tenant", "acme" } } });
  require(exposed && exposed.value->artifacts.front().parts.front().text == "local:input" &&
      local_client.get({ .id = "local-task" }) &&
      observed_metadata.at("trace_id") == "request-trace" &&
      observed_metadata.at("attempt") == "7" &&
      observed_metadata.at("tenant") == "acme",
    "local teams preserve A2A request metadata with deterministic overrides");
}

void adapter_capabilities_and_function_handlers_are_conservative() {
  auto conservative_client = std::make_shared<a2a::client>(
    std::make_shared<conservative_transport>());
  a2a::remote_agent_executor conservative(conservative_client);
  require(!conservative.capabilities().concurrent_execution,
    "remote executors do not assume custom transports are thread safe");

  auto concurrent_client = std::make_shared<a2a::client>(
    std::make_shared<a2a::in_process_transport>(
      std::make_shared<a2a::service>(
        test_card(), std::make_shared<memory_handler>())));
  a2a::remote_agent_executor concurrent(concurrent_client);
  require(concurrent.capabilities().concurrent_execution,
    "known thread-safe transports advertise concurrent invocation explicitly");

  bool rejected = false;
  try {
    (void)a2a::function_task_handler({}, {}, {});
  }
  catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "function task handlers fail fast on empty callbacks");
}

void non_blocking_tasks_can_be_observed_and_cancelled() {
  auto registry = std::make_shared<ma::agent_registry>();
  std::atomic<bool> entered { false };
  registry->add({ .id = "worker", .name = "worker" },
    std::make_shared<ma::function_agent_executor>(
      [&](const ma::agent_task_request&, const ma::agent_execution_context& context) {
        entered = true;
        while (!context.stop_token.stop_requested()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return ma::agent_task_result {
          .status = ma::agent_task_status::cancelled,
          .error_code = ma::agent_task_error_code::cancelled,
          .error = "cancelled",
        };
      },
      ma::agent_executor_capabilities {
        .cooperative_cancellation = true,
        .concurrent_execution = false,
      }));
  auto runtime = std::make_shared<ma::team_runtime>(ma::team_runtime_options {
    .registry = registry,
  });
  auto handler = std::make_shared<a2a::team_task_handler>(a2a::team_task_handler_options {
    .runtime = runtime,
    .preferred_agent = "worker",
  });
  a2a::client client(std::make_shared<a2a::in_process_transport>(
    std::make_shared<a2a::service>(test_card(), handler)));

  const auto submitted = client.send({
    .value = {
      .message_id = "async-message",
      .parts = { a2a::part::text_part("wait") },
      .task_id = "async-task",
    },
    .configuration = { .blocking = false },
  });
  require(submitted && submitted.value->status.state == a2a::task_state::submitted,
    "non-blocking message/send returns a submitted task");
  while (!entered) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const auto cancelled = client.cancel({ .id = "async-task" });
  require(cancelled && cancelled.value->status.state == a2a::task_state::canceled,
    "tasks/cancel signals a running local team task");
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto terminal = client.cancel({ .id = "async-task" });
    if (!terminal && terminal.failure->code == a2a::error_code::task_not_cancelable) {
      const auto current = client.get({ .id = "async-task" });
      require(current && current.value->status.state == a2a::task_state::canceled,
        "a successful cancellation remains terminal after background completion");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  throw std::runtime_error("cancelled A2A task did not reach a terminal state");
}

void input_required_tasks_can_continue_or_cancel() {
  auto registry = std::make_shared<ma::agent_registry>();
  registry->add({ .id = "interactive", .name = "interactive" },
    std::make_shared<ma::function_agent_executor>(
      [](const ma::agent_task_request& request, const ma::agent_execution_context&) {
        if (request.input != "details") {
          return ma::agent_task_result {
            .status = ma::agent_task_status::input_required,
            .output = "partial",
            .error = "provide details",
            .artifacts = { {
              .id = "draft",
              .name = "draft",
              .content = "v1",
            } },
          };
        }
        return ma::agent_task_result {
          .output = "final",
          .artifacts = { {
            .id = "draft",
            .name = "draft",
            .content = "v2",
          } },
        };
      }));
  auto runtime = std::make_shared<ma::team_runtime>(ma::team_runtime_options {
    .registry = registry,
  });
  auto handler = std::make_shared<a2a::team_task_handler>(a2a::team_task_handler_options {
    .runtime = runtime,
    .preferred_agent = "interactive",
  });
  a2a::client client(std::make_shared<a2a::in_process_transport>(
    std::make_shared<a2a::service>(test_card(), handler)));

  const auto paused = client.send({ .value = {
    .message_id = "interactive-start",
    .parts = { a2a::part::text_part("start") },
    .task_id = "interactive-task",
    .context_id = "interactive-context",
  } });
  require(paused && paused.value->status.state == a2a::task_state::input_required,
    "local input-required results remain resumable A2A tasks");
  const auto resumed = client.send({ .value = {
    .message_id = "interactive-details",
    .parts = { a2a::part::text_part("details") },
    .task_id = "interactive-task",
  } });
  require(resumed && resumed.value->status.state == a2a::task_state::completed &&
      resumed.value->history.size() == 2 && resumed.value->artifacts.size() == 2 &&
      resumed.value->artifacts.front().parts.front().text == "final" &&
      resumed.value->artifacts.back().parts.front().text == "v2" &&
      !resumed.value->status.status_message,
    "continuations inherit context, append history, and replace artifacts by id");
  const auto recent = client.get({ .id = "interactive-task", .history_length = 1 });
  require(recent && recent.value->history.size() == 1 &&
      recent.value->history.front().message_id == "interactive-details",
    "continued tasks honor bounded history queries");

  const auto paused_for_cancel = client.send({ .value = {
    .message_id = "cancel-paused-message",
    .parts = { a2a::part::text_part("pause") },
    .task_id = "cancel-paused-task",
  } });
  const auto cancelled = client.cancel({ .id = "cancel-paused-task" });
  require(paused_for_cancel &&
      paused_for_cancel.value->status.state == a2a::task_state::input_required &&
      cancelled && cancelled.value->status.state == a2a::task_state::canceled,
    "input-required tasks can be cancelled while no executor lease is active");
}

void in_process_transport_honors_pre_cancelled_requests() {
  std::atomic<int> sends { 0 };
  auto handler = std::make_shared<a2a::function_task_handler>(
    [&](const a2a::send_message_params&, std::stop_token) {
      ++sends;
      return a2a::result<a2a::task> { .value = a2a::task {} };
    },
    [](const a2a::task_query_params&) {
      return a2a::result<a2a::task> { .value = a2a::task {} };
    },
    [](const a2a::task_id_params&) {
      return a2a::result<a2a::task> { .value = a2a::task {} };
    });
  a2a::client client(std::make_shared<a2a::in_process_transport>(
    std::make_shared<a2a::service>(test_card(), std::move(handler))));
  std::stop_source stop_source;
  stop_source.request_stop();
  const auto result = client.send({ .value = {
    .message_id = "cancelled-message",
    .parts = { a2a::part::text_part("ignored") },
  } }, stop_source.get_token());
  require(!result && result.failure->code == a2a::error_code::transport_error && sends == 0,
    "in-process transport does not invoke handlers for pre-cancelled requests");
}

void non_blocking_handler_applies_background_backpressure() {
  std::atomic<bool> entered { false };
  std::atomic<bool> release { false };
  auto registry = std::make_shared<ma::agent_registry>();
  registry->add({ .id = "slow", .name = "slow" },
    std::make_shared<ma::function_agent_executor>(
      [&](const ma::agent_task_request&, const ma::agent_execution_context&) {
        entered = true;
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return ma::agent_task_result { .output = "done" };
      }));
  auto runtime = std::make_shared<ma::team_runtime>(ma::team_runtime_options {
    .registry = registry,
  });
  auto handler = std::make_shared<a2a::team_task_handler>(a2a::team_task_handler_options {
    .runtime = runtime,
    .preferred_agent = "slow",
    .max_background_tasks = 1,
  });
  a2a::client client(std::make_shared<a2a::in_process_transport>(
    std::make_shared<a2a::service>(test_card(), handler)));
  const auto first = client.send({
    .value = {
      .message_id = "background-one",
      .parts = { a2a::part::text_part("first") },
      .task_id = "background-one",
    },
    .configuration = { .blocking = false },
  });
  while (!entered) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const auto second = client.send({
    .value = {
      .message_id = "background-two",
      .parts = { a2a::part::text_part("second") },
      .task_id = "background-two",
    },
    .configuration = { .blocking = false },
  });
  release = true;
  require(first && !second && second.failure->code == a2a::error_code::internal_error &&
      second.failure->data &&
      second.failure->data->at("reason") == "background_capacity_exhausted" &&
      second.failure->data->at("retryable").get<bool>(),
    "non-blocking A2A tasks apply bounded backpressure instead of spawning without limit");
}

void run(const char* name, void (*test)()) {
  test();
  wuwe::println("[PASS] {}", name);
}
} // namespace

int main() {
  try {
    run("codecs and in-process protocol work", codecs_and_in_process_protocol_work);
    run("HTTP client and service adapters work", http_client_and_service_adapters_work);
    run("service rejects unsupported advertised capabilities",
      service_rejects_unsupported_advertised_capabilities);
    run("JSON-RPC notifications and invalid requests are handled",
      jsonrpc_notifications_and_invalid_requests_are_handled);
    run("remote and local team adapters bridge A2A", remote_and_local_team_adapters_bridge_a2a);
    run("adapter capabilities are conservative",
      adapter_capabilities_and_function_handlers_are_conservative);
    run("non-blocking tasks can be observed and cancelled",
      non_blocking_tasks_can_be_observed_and_cancelled);
    run("input-required tasks can continue or cancel",
      input_required_tasks_can_continue_or_cancel);
    run("in-process transport honors pre-cancelled requests",
      in_process_transport_honors_pre_cancelled_requests);
    run("non-blocking handler applies background backpressure",
      non_blocking_handler_applies_background_backpressure);
  }
  catch (const std::exception& ex) {
    wuwe::println("[FAIL] {}", ex.what());
    return 1;
  }
}
