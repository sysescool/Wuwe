#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <wuwe/agent/llm/dispatching_llm_client.hpp>
#include <wuwe/agent/reasoning/reasoning.hpp>
#include <wuwe/agent/routing/routing.hpp>
#include <wuwe/agent/runtime/llm_continuation.hpp>

using namespace wuwe;
namespace llm = wuwe::agent::llm;
namespace reasoning = wuwe::agent::reasoning;
namespace routing = wuwe::agent::routing;
namespace runtime = wuwe::agent::runtime;

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class recording_client final : public llm_client {
public:
  explicit recording_client(
    std::string output,
    llm_provider_capabilities capabilities = {
      .streaming = true,
      .tools = true,
      .tool_choice = true,
      .json_response_format = true,
      .stop_sequences = true,
      .deterministic_seed = true,
      .json_schema_output = true,
      .explicit_cache_control = true,
    })
      : output_(std::move(output)), capabilities_(capabilities) {
  }

  llm_response complete(const llm_request& request) override {
    return complete(request, {});
  }

  llm_response complete(const llm_request& request, std::stop_token stop_token) override {
    if (stop_token.stop_requested()) {
      return {
        .error_code = agent::make_error_code(agent::llm_error_code::cancelled),
      };
    }
    record(request);
    return { .content = output_ };
  }

  bool supports_streaming() const noexcept override {
    return capabilities_.streaming;
  }

  [[nodiscard]] llm_provider_capabilities capabilities() const noexcept override {
    return capabilities_;
  }

  llm_response complete_stream(const llm_request& request, const llm_stream_callbacks& callbacks,
    std::stop_token stop_token = {}) override {
    if (stop_token.stop_requested()) {
      return {
        .error_code = agent::make_error_code(agent::llm_error_code::cancelled),
      };
    }
    record(request);
    llm_response response { .content = output_ };
    if (callbacks.on_event) {
      callbacks.on_event({
        .type = llm_stream_event_type::content_delta,
        .content_delta = output_,
      });
      callbacks.on_event({
        .type = llm_stream_event_type::done,
        .response = response,
      });
    }
    return response;
  }

  [[nodiscard]] std::vector<llm_request> requests() const {
    std::scoped_lock lock(mutex_);
    return requests_;
  }

private:
  void record(const llm_request& request) {
    std::scoped_lock lock(mutex_);
    requests_.push_back(request);
  }

  std::string output_;
  llm_provider_capabilities capabilities_;
  mutable std::mutex mutex_;
  std::vector<llm_request> requests_;
};

class throwing_client final : public llm_client {
public:
  llm_response complete(const llm_request&) override {
    throw std::runtime_error("backend failure");
  }
};

class failing_graph_dependency_client final : public llm_client,
                                              public llm::llm_client_registry_dependency {
public:
  llm_response complete(const llm_request&) override {
    return { .content = "unused" };
  }

  [[nodiscard]] bool references_registry(
    const llm::llm_client_registry*, std::vector<const void*>&) const override {
    throw std::runtime_error("graph validation unavailable");
  }
};

class forwarding_client final : public llm_client {
public:
  explicit forwarding_client(const std::shared_ptr<llm_client>& target) : target_(target) {
  }

  llm_response complete(const llm_request& request) override {
    const auto target = target_.lock();
    if (!target) {
      return {
        .content = "forwarding target expired",
        .error_code = agent::make_error_code(agent::llm_error_code::model_unavailable),
      };
    }
    return target->complete(request);
  }

  bool supports_streaming() const noexcept override {
    const auto target = target_.lock();
    return target && target->supports_streaming();
  }

  [[nodiscard]] llm_provider_capabilities capabilities() const noexcept override {
    const auto target = target_.lock();
    return target ? target->capabilities() : llm_provider_capabilities {};
  }

  llm_response complete_stream(const llm_request& request, const llm_stream_callbacks& callbacks,
    std::stop_token stop_token = {}) override {
    const auto target = target_.lock();
    if (!target) {
      return {
        .content = "forwarding target expired",
        .error_code = agent::make_error_code(agent::llm_error_code::model_unavailable),
      };
    }
    return target->complete_stream(request, callbacks, stop_token);
  }

private:
  std::weak_ptr<llm_client> target_;
};

class threaded_forwarding_client final : public llm_client {
public:
  explicit threaded_forwarding_client(const std::shared_ptr<llm_client>& target) : target_(target) {
  }

  llm_response complete(const llm_request& request) override {
    const auto target = target_.lock();
    if (!target) {
      return {
        .content = "forwarding target expired",
        .error_code = agent::make_error_code(agent::llm_error_code::model_unavailable),
      };
    }
    return std::async(std::launch::async, [target, request] {
      return target->complete(request);
    }).get();
  }

  [[nodiscard]] llm_provider_capabilities capabilities() const noexcept override {
    return { .declared = false };
  }

private:
  std::weak_ptr<llm_client> target_;
};

class concurrent_stream_client final : public llm_client {
public:
  explicit concurrent_stream_client(std::size_t event_count) : event_count_(event_count) {
  }

  llm_response complete(const llm_request&) override {
    return { .content = "concurrent" };
  }

  bool supports_streaming() const noexcept override {
    return true;
  }

  [[nodiscard]] llm_provider_capabilities capabilities() const noexcept override {
    return { .streaming = true };
  }

  llm_response complete_stream(
    const llm_request&, const llm_stream_callbacks& callbacks, std::stop_token = {}) override {
    std::vector<std::future<void>> emissions;
    emissions.reserve(event_count_);
    for (std::size_t index = 0; index < event_count_; ++index) {
      emissions.push_back(std::async(std::launch::async, [callbacks, index] {
        if (callbacks.on_event) {
          callbacks.on_event({
            .type = llm_stream_event_type::content_delta,
            .content_delta = std::to_string(index),
          });
        }
      }));
    }
    for (auto& emission : emissions) {
      emission.get();
    }
    llm_response response { .content = "concurrent" };
    if (callbacks.on_event) {
      callbacks.on_event({
        .type = llm_stream_event_type::done,
        .response = response,
      });
    }
    return response;
  }

private:
  std::size_t event_count_ {};
};

void registry_is_explicit_and_hot_swappable() {
  auto registry = std::make_shared<llm::llm_client_registry>();
  auto first = std::make_shared<recording_client>("first");
  registry->add({ .provider = "primary", .client = first });

  bool duplicate_rejected = false;
  try {
    registry->add({
      .provider = "primary",
      .client = std::make_shared<recording_client>("duplicate"),
    });
  }
  catch (const std::invalid_argument&) {
    duplicate_rejected = true;
  }
  require(duplicate_rejected, "registry rejects duplicate provider ids");

  llm::dispatching_llm_client dispatcher(registry);
  llm_request request;
  request.model = "model-a";
  request.messages.push_back({ .role = "user", .content = "hello" });
  const auto first_response = dispatcher.complete(request);
  require(first_response && first_response.content == "first",
    "a single registered client is an unambiguous fallback");
  const auto first_requests = first->requests();
  require(first_requests.size() == 1 && first_requests.front().provider == "primary" &&
            first_requests.front().runtime_context && !request.runtime_context,
    "dispatch forwards the resolved provider explicitly");
  runtime::llm_tool_continuation continuation;
  continuation.request = first_requests.front();
  const auto restored =
    runtime::llm_continuation_from_json(runtime::llm_continuation_to_json(continuation));
  require(!restored.request.runtime_context,
    "opaque LLM runtime context is never persisted in durable continuations");

  auto second = std::make_shared<recording_client>("second");
  const auto previous = registry->replace({
    .provider = "primary",
    .client = second,
  });
  require(previous == first, "registry replacement returns the previous owned client");
  const auto second_response = dispatcher.complete(request);
  require(second_response && second_response.content == "second" && second->requests().size() == 1,
    "new calls observe an atomic client replacement");

  const auto removed = registry->remove("primary");
  require(removed == second && registry->empty(),
    "registry removal returns ownership and leaves no stale binding");
  const auto unavailable = dispatcher.complete(request);
  require(!unavailable && unavailable.error_code == agent::llm_error_code::model_unavailable,
    "an empty registry returns a stable model-unavailable result");

  auto recursive_registry = std::make_shared<llm::llm_client_registry>();
  auto recursive_dispatcher = std::make_shared<llm::dispatching_llm_client>(recursive_registry);
  std::weak_ptr<llm_client> recursive_lifetime = recursive_dispatcher;
  bool recursive_rejected = false;
  try {
    recursive_registry->add({
      .provider = "recursive",
      .client = recursive_dispatcher,
    });
  }
  catch (const std::invalid_argument&) {
    recursive_rejected = true;
  }
  recursive_dispatcher.reset();
  require(recursive_rejected && recursive_lifetime.expired(),
    "the registry rejects direct dispatcher ownership cycles before retaining them");
}

void indirect_dispatch_cycles_fail_without_recursion() {
  auto first_registry = std::make_shared<llm::llm_client_registry>();
  auto second_registry = std::make_shared<llm::llm_client_registry>();
  auto first = std::make_shared<llm::dispatching_llm_client>(first_registry);
  auto second = std::make_shared<llm::dispatching_llm_client>(second_registry);
  first_registry->add({ .provider = "cycle", .client = second });
  bool strong_cycle_rejected = false;
  try {
    second_registry->add({ .provider = "cycle", .client = first });
  }
  catch (const std::invalid_argument&) {
    strong_cycle_rejected = true;
  }
  require(
    strong_cycle_rejected, "the registry rejects indirect dispatcher ownership cycles atomically");
  (void)first_registry->remove("cycle");

  first_registry->add({
    .provider = "cycle",
    .client = std::make_shared<forwarding_client>(second),
  });
  second_registry->add({
    .provider = "cycle",
    .client = std::make_shared<forwarding_client>(first),
  });

  require(!first->capabilities().declared && !first->supports_streaming(),
    "cyclic dispatcher graphs do not recurse during capability discovery");

  llm_request request;
  request.provider = "cycle";
  request.model = "cycle-model";
  const auto response = first->complete(request);
  require(!response && response.error_code == agent::llm_error_code::invalid_request &&
            response.content == "LLM dispatch cycle detected.",
    "indirect dispatcher cycles return a stable error instead of overflowing the stack");

  bool stream_error_emitted = false;
  const auto stream_response = first->complete_stream(request,
    {
      .on_event =
        [&](const llm_stream_event& event) {
          stream_error_emitted =
            stream_error_emitted || (event.type == llm_stream_event_type::error &&
                                      event.error_code == agent::llm_error_code::invalid_request);
        },
    });
  require(!stream_response && stream_error_emitted &&
            stream_response.error_code == agent::llm_error_code::invalid_request,
    "streaming dispatch cycles return the same stable terminal error");

  (void)first_registry->remove("cycle");
  (void)second_registry->remove("cycle");
}

void cross_thread_dispatch_cycles_preserve_request_lineage() {
  auto first_registry = std::make_shared<llm::llm_client_registry>();
  auto second_registry = std::make_shared<llm::llm_client_registry>();
  auto first = std::make_shared<llm::dispatching_llm_client>(first_registry);
  auto second = std::make_shared<llm::dispatching_llm_client>(second_registry);
  first_registry->add({
    .provider = "cycle",
    .client = std::make_shared<threaded_forwarding_client>(second),
  });
  second_registry->add({
    .provider = "cycle",
    .client = std::make_shared<threaded_forwarding_client>(first),
  });

  llm_request request;
  request.provider = "cycle";
  const auto response = first->complete(request);
  require(!response && response.error_code == agent::llm_error_code::invalid_request &&
            response.content == "LLM dispatch cycle detected.",
    "request lineage detects dispatcher cycles that cross worker threads");

  (void)first_registry->remove("cycle");
  (void)second_registry->remove("cycle");
}

void concurrent_registry_mutations_cannot_commit_a_cycle() {
  auto first_registry = std::make_shared<llm::llm_client_registry>();
  auto second_registry = std::make_shared<llm::llm_client_registry>();
  auto first = std::make_shared<llm::dispatching_llm_client>(first_registry);
  auto second = std::make_shared<llm::dispatching_llm_client>(second_registry);

  std::promise<void> start_promise;
  const auto start = start_promise.get_future().share();
  auto add_first = std::async(std::launch::async, [&, start] {
    start.wait();
    try {
      first_registry->add({ .provider = "cycle", .client = second });
      return true;
    }
    catch (const std::invalid_argument&) {
      return false;
    }
  });
  auto add_second = std::async(std::launch::async, [&, start] {
    start.wait();
    try {
      second_registry->add({ .provider = "cycle", .client = first });
      return true;
    }
    catch (const std::invalid_argument&) {
      return false;
    }
  });
  start_promise.set_value();

  const auto first_added = add_first.get();
  const auto second_added = add_second.get();
  require(first_added != second_added,
    "concurrent reciprocal registry mutations commit exactly one acyclic edge");

  (void)first_registry->remove("cycle");
  (void)second_registry->remove("cycle");
}

void graph_validation_failures_abort_registry_mutations() {
  auto registry = std::make_shared<llm::llm_client_registry>();
  auto original = std::make_shared<recording_client>("original");
  registry->add({ .provider = "primary", .client = original });

  bool replace_failed = false;
  try {
    (void)registry->replace({
      .provider = "primary",
      .client = std::make_shared<failing_graph_dependency_client>(),
    });
  }
  catch (const std::runtime_error&) {
    replace_failed = true;
  }
  require(replace_failed && registry->find("primary") == original,
    "a graph-validation failure leaves the existing binding unchanged");

  bool add_failed = false;
  try {
    registry->add({
      .provider = "secondary",
      .client = std::make_shared<failing_graph_dependency_client>(),
    });
  }
  catch (const std::runtime_error&) {
    add_failed = true;
  }
  require(add_failed && !registry->find("secondary") && registry->size() == 1,
    "a graph-validation failure cannot commit a new binding");
}

void multiple_clients_require_an_explicit_provider() {
  auto registry = std::make_shared<llm::llm_client_registry>();
  auto alpha = std::make_shared<recording_client>("alpha");
  auto beta = std::make_shared<recording_client>("beta");
  registry->add({ .provider = "alpha", .client = alpha });
  registry->add({ .provider = "beta", .client = beta });
  llm::dispatching_llm_client dispatcher(registry);

  llm_request ambiguous;
  ambiguous.model = "shared-name";
  ambiguous.messages.push_back({ .role = "user", .content = "hello" });
  const auto rejected = dispatcher.complete(ambiguous);
  require(!rejected && rejected.error_code == agent::llm_error_code::invalid_request &&
            alpha->requests().empty() && beta->requests().empty(),
    "dispatch never silently chooses among multiple providers");

  ambiguous.provider = "beta";
  const auto selected = dispatcher.complete(ambiguous);
  require(selected && selected.content == "beta" && beta->requests().size() == 1 &&
            selected.metadata.at("wuwe.dispatch.provider") == "beta",
    "an explicit provider selects and reports the matching client");

  ambiguous.provider = "missing";
  const auto missing = dispatcher.complete(ambiguous);
  require(!missing && missing.error_code == agent::llm_error_code::model_unavailable &&
            missing.metadata.at("wuwe.dispatch.provider") == "missing",
    "unknown providers fail without falling back to a different client");

  llm::dispatching_llm_client defaulted(registry,
    {
      .default_provider = "alpha",
    });
  ambiguous.provider.clear();
  const auto default_response = defaulted.complete(ambiguous);
  require(default_response && default_response.content == "alpha" && alpha->requests().size() == 1,
    "an explicit default provider resolves otherwise ambiguous requests");
}

void selected_client_capabilities_are_enforced() {
  auto registry = std::make_shared<llm::llm_client_registry>();
  auto capable = std::make_shared<recording_client>("capable");
  auto limited = std::make_shared<recording_client>("limited", llm_provider_capabilities {});
  registry->add({ .provider = "capable", .client = capable });
  registry->add({ .provider = "limited", .client = limited });
  llm::dispatching_llm_client dispatcher(registry);

  require(dispatcher.capabilities().tools,
    "aggregate capabilities describe what the dispatch set can provide");

  llm_request request;
  request.provider = "limited";
  request.model = "limited-model";
  request.messages.push_back({ .role = "user", .content = "use a tool" });
  request.tools.push_back({
    .name = "lookup",
    .description = "Look up a value",
    .parameters_json_schema = R"({"type":"object"})",
  });
  const auto rejected = dispatcher.complete(request);
  require(!rejected && rejected.error_code == agent::llm_error_code::unsupported_capability &&
            limited->requests().empty(),
    "the selected provider contract is validated before backend execution");
}

void dispatch_observation_is_isolated_and_reports_backend_exceptions() {
  auto healthy_registry = std::make_shared<llm::llm_client_registry>();
  healthy_registry->add({
    .provider = "healthy",
    .client = std::make_shared<recording_client>("ok"),
  });
  llm::dispatching_llm_client healthy(healthy_registry,
    {
      .observer =
        [](const llm::llm_dispatch_event&) { throw std::runtime_error("telemetry unavailable"); },
    });
  llm_request request;
  request.provider = "healthy";
  const auto response = healthy.complete(request);
  require(response && response.content == "ok",
    "dispatch observer failures do not alter model execution");

  auto failing_registry = std::make_shared<llm::llm_client_registry>();
  failing_registry->add({
    .provider = "failing",
    .client = std::make_shared<throwing_client>(),
  });
  std::vector<llm::llm_dispatch_event> events;
  llm::dispatching_llm_client failing(failing_registry,
    {
      .observer = [&](const llm::llm_dispatch_event& event) { events.push_back(event); },
    });
  request.provider = "failing";
  bool propagated = false;
  try {
    (void)failing.complete(request);
  }
  catch (const std::runtime_error&) {
    propagated = true;
  }
  require(propagated && events.size() == 2 &&
            events.front().type == llm::llm_dispatch_event_type::client_selected &&
            events.back().type == llm::llm_dispatch_event_type::request_failed &&
            events.back().error_code == agent::llm_error_code::api_error,
    "backend exceptions preserve behavior and publish a terminal failure event");
}

void observer_started_requests_do_not_recurse_observation() {
  auto registry = std::make_shared<llm::llm_client_registry>();
  auto client = std::make_shared<recording_client>("ok");
  registry->add({ .provider = "primary", .client = client });

  llm::dispatching_llm_client* active_dispatcher = nullptr;
  bool nested_started = false;
  std::size_t observer_calls = 0;
  llm_response nested_response;
  llm::dispatching_llm_client dispatcher(registry,
    {
      .observer =
        [&](const llm::llm_dispatch_event&) {
          ++observer_calls;
          if (nested_started) {
            return;
          }
          nested_started = true;
          llm_request nested_request;
          nested_request.provider = "primary";
          nested_response = active_dispatcher->complete(nested_request);
        },
    });
  active_dispatcher = &dispatcher;

  llm_request request;
  request.provider = "primary";
  const auto response = dispatcher.complete(request);
  require(response && nested_response && client->requests().size() == 2 && observer_calls == 2,
    "observer-started requests execute without recursively invoking the same observer");
}

void streaming_callback_failures_are_not_backend_failures() {
  auto registry = std::make_shared<llm::llm_client_registry>();
  registry->add({
    .provider = "streaming",
    .client = std::make_shared<recording_client>("streamed"),
  });
  std::vector<llm::llm_dispatch_event> events;
  llm::dispatching_llm_client dispatcher(registry,
    {
      .observer = [&](const llm::llm_dispatch_event& event) { events.push_back(event); },
    });

  llm_request request;
  request.provider = "streaming";
  bool callback_failure_propagated = false;
  try {
    (void)dispatcher.complete_stream(request,
      {
        .on_event = [](const llm_stream_event&) { throw std::runtime_error("consumer failed"); },
      });
  }
  catch (const std::runtime_error& error) {
    callback_failure_propagated = std::string(error.what()) == "consumer failed";
  }
  require(callback_failure_propagated && events.size() == 2 &&
            events.front().type == llm::llm_dispatch_event_type::client_selected &&
            events.back().type == llm::llm_dispatch_event_type::request_completed,
    "stream consumer failures propagate without being misreported as backend API failures");
}

void independent_stream_callback_requests_are_not_cycles() {
  auto registry = std::make_shared<llm::llm_client_registry>();
  auto client = std::make_shared<recording_client>("ok");
  registry->add({ .provider = "primary", .client = client });
  llm::dispatching_llm_client dispatcher(registry);

  bool nested_started = false;
  llm_response nested_response;
  llm_request outer_request;
  outer_request.provider = "primary";
  const auto outer_response = dispatcher.complete_stream(outer_request,
    {
      .on_event =
        [&](const llm_stream_event& event) {
          if (event.type != llm_stream_event_type::content_delta || nested_started) {
            return;
          }
          nested_started = true;
          llm_request nested_request;
          nested_request.provider = "primary";
          nested_response = dispatcher.complete(nested_request);
        },
    });

  require(outer_response && nested_response && client->requests().size() == 2,
    "an independent request started by a stream consumer is not a dispatch cycle");
}

void concurrent_stream_events_are_serialized_for_consumers() {
  constexpr std::size_t event_count = 24;
  auto registry = std::make_shared<llm::llm_client_registry>();
  registry->add({
    .provider = "concurrent",
    .client = std::make_shared<concurrent_stream_client>(event_count),
  });
  llm::dispatching_llm_client dispatcher(registry);

  std::atomic<int> active_callbacks { 0 };
  std::atomic<int> maximum_active { 0 };
  std::atomic<std::size_t> observed_events { 0 };
  llm_request request;
  request.provider = "concurrent";
  const auto response = dispatcher.complete_stream(request,
    {
      .on_event =
        [&](const llm_stream_event& event) {
          if (event.type != llm_stream_event_type::content_delta) {
            return;
          }
          const auto active = active_callbacks.fetch_add(1) + 1;
          auto maximum = maximum_active.load();
          while (active > maximum && !maximum_active.compare_exchange_weak(maximum, active)) {
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          observed_events.fetch_add(1);
          active_callbacks.fetch_sub(1);
        },
    });

  require(response && observed_events.load() == event_count && maximum_active.load() == 1,
    "concurrent provider events are serialized before consumer callbacks run");
}

void streaming_and_concurrent_dispatch_preserve_provider_identity() {
  auto registry = std::make_shared<llm::llm_client_registry>();
  auto alpha = std::make_shared<recording_client>("alpha");
  auto beta = std::make_shared<recording_client>("beta");
  registry->add({ .provider = "alpha", .client = alpha });
  registry->add({ .provider = "beta", .client = beta });
  llm::dispatching_llm_client dispatcher(registry);

  bool saw_delta = false;
  bool saw_annotated_done = false;
  llm_request streaming;
  streaming.provider = "alpha";
  streaming.model = "stream-model";
  streaming.messages.push_back({ .role = "user", .content = "stream" });
  const auto streamed = dispatcher.complete_stream(streaming,
    {
      .on_event =
        [&](const llm_stream_event& event) {
          saw_delta = saw_delta || event.type == llm_stream_event_type::content_delta;
          saw_annotated_done = saw_annotated_done ||
                               (event.type == llm_stream_event_type::done && event.response &&
                                 event.response->metadata.contains("wuwe.dispatch.provider") &&
                                 event.response->metadata.at("wuwe.dispatch.provider") == "alpha");
        },
    });
  require(streamed && saw_delta && saw_annotated_done && alpha->requests().size() == 1,
    "streaming callbacks retain dispatch identity through terminal events");

  std::vector<std::future<llm_response>> calls;
  for (int index = 0; index < 32; ++index) {
    calls.push_back(std::async(std::launch::async, [&, index] {
      llm_request request;
      request.provider = index % 2 == 0 ? "alpha" : "beta";
      request.model = "parallel-model";
      request.messages.push_back({ .role = "user", .content = "parallel" });
      return dispatcher.complete(request);
    }));
  }
  for (auto& call : calls) {
    require(static_cast<bool>(call.get()), "concurrent dispatch calls complete successfully");
  }
  require(alpha->requests().size() == 17 && beta->requests().size() == 16,
    "concurrent calls remain isolated by provider");
}

routing::model_resource_profile routed_profile(
  std::string model, std::string provider, double quality, double cost) {
  return {
    .model = std::move(model),
    .provider = std::move(provider),
    .context_window_tokens = 16'000,
    .max_output_tokens = 2'000,
    .input_cost_per_million_tokens = cost,
    .output_cost_per_million_tokens = cost,
    .quality_score = quality,
    .latency_score = 0.8,
    .capabilities = { .streaming = true },
  };
}

void reasoning_routes_to_the_selected_provider_client() {
  auto registry = std::make_shared<llm::llm_client_registry>();
  auto economy = std::make_shared<recording_client>("economy answer");
  auto premium = std::make_shared<recording_client>("premium answer");
  registry->add({ .provider = "economy-provider", .client = economy });
  registry->add({ .provider = "premium-provider", .client = premium });
  llm::dispatching_llm_client dispatcher(registry);

  reasoning::reasoning_runner direct_runner(dispatcher);
  const auto direct = direct_runner.run({
    .input = "use the requested provider",
    .model = "economy-model",
    .policy = {
      .mode = reasoning::reasoning_mode::simple,
      .enable_streaming = false,
    },
    .provider = "economy-provider",
  });
  require(direct.completed && direct.content == "economy answer" && economy->requests().size() == 1,
    "Reasoning propagates an explicitly requested provider without a router");

  auto router = std::make_shared<routing::resource_aware_router>();
  router->add(routed_profile("economy-model", "economy-provider", 0.5, 0.1));
  router->add(routed_profile("premium-model", "premium-provider", 0.95, 2.0));

  reasoning::reasoning_runner runner({
    .client = &dispatcher,
    .model_router = router,
  });
  const auto result = runner.run({
    .input = "answer carefully",
    .model = "economy-model",
    .policy = {
      .mode = reasoning::reasoning_mode::simple,
      .enable_streaming = false,
    },
    .model_routing = {
      .strategy = routing::model_selection_strategy::highest_quality,
    },
  });

  require(result.completed && result.content == "premium answer" &&
            economy->requests().size() == 1 && premium->requests().size() == 1,
    "Reasoning executes through the provider selected by resource routing");
  require(premium->requests().front().model == "premium-model" &&
            premium->requests().front().provider == "premium-provider" &&
            result.model_routes.size() == 1 &&
            result.model_routes.front().selected_provider == "premium-provider",
    "the routed model and provider remain visible end to end");

  const auto pinned = runner.run({
    .input = "stay on the explicitly selected provider",
    .model = "economy-model",
    .policy = {
      .mode = reasoning::reasoning_mode::simple,
      .enable_streaming = false,
    },
    .model_routing = {
      .strategy = routing::model_selection_strategy::highest_quality,
    },
    .provider = "economy-provider",
  });
  require(pinned.completed && pinned.content == "economy answer" &&
            economy->requests().size() == 2 && premium->requests().size() == 1,
    "Reasoning pins an explicitly selected provider by default");

  const auto override_allowed = runner.run({
    .input = "allow the router to change providers",
    .model = "economy-model",
    .policy = {
      .mode = reasoning::reasoning_mode::simple,
      .enable_streaming = false,
    },
    .model_routing = {
      .strategy = routing::model_selection_strategy::highest_quality,
      .allow_provider_override = true,
    },
    .provider = "economy-provider",
  });
  require(override_allowed.completed && override_allowed.content == "premium answer" &&
            premium->requests().size() == 2,
    "Reasoning changes an explicit provider only with an explicit routing opt-in");
}

} // namespace

int main() {
  const auto run = [](const char* name, auto&& test) {
    std::cerr << "[ RUN      ] " << name << '\n';
    try {
      test();
    }
    catch (const std::exception& ex) {
      std::cerr << "[  FAILED  ] " << name << ": " << ex.what() << '\n';
      throw;
    }
    std::cerr << "[       OK ] " << name << '\n';
  };
  run("registry_is_explicit_and_hot_swappable", registry_is_explicit_and_hot_swappable);
  run("indirect_dispatch_cycles_fail_without_recursion",
    indirect_dispatch_cycles_fail_without_recursion);
  run("cross_thread_dispatch_cycles_preserve_request_lineage",
    cross_thread_dispatch_cycles_preserve_request_lineage);
  run("concurrent_registry_mutations_cannot_commit_a_cycle",
    concurrent_registry_mutations_cannot_commit_a_cycle);
  run("graph_validation_failures_abort_registry_mutations",
    graph_validation_failures_abort_registry_mutations);
  run(
    "multiple_clients_require_an_explicit_provider", multiple_clients_require_an_explicit_provider);
  run("selected_client_capabilities_are_enforced", selected_client_capabilities_are_enforced);
  run("dispatch_observation_is_isolated_and_reports_backend_exceptions",
    dispatch_observation_is_isolated_and_reports_backend_exceptions);
  run("observer_started_requests_do_not_recurse_observation",
    observer_started_requests_do_not_recurse_observation);
  run("streaming_callback_failures_are_not_backend_failures",
    streaming_callback_failures_are_not_backend_failures);
  run("independent_stream_callback_requests_are_not_cycles",
    independent_stream_callback_requests_are_not_cycles);
  run("concurrent_stream_events_are_serialized_for_consumers",
    concurrent_stream_events_are_serialized_for_consumers);
  run("streaming_and_concurrent_dispatch_preserve_provider_identity",
    streaming_and_concurrent_dispatch_preserve_provider_identity);
  run("reasoning_routes_to_the_selected_provider_client",
    reasoning_routes_to_the_selected_provider_client);
}
