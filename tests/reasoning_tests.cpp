#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <wuwe/agent/reasoning/reasoning.hpp>
#include <wuwe/agent/routing/routing.hpp>
#include <wuwe/common/print.h>

namespace {

using namespace wuwe;
namespace planning = wuwe::agent::planning;
namespace reasoning = wuwe::agent::reasoning;
namespace reflection = wuwe::agent::reflection;
namespace guardrails = wuwe::agent::guardrails;
namespace routing = wuwe::agent::routing;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class scripted_llm_client final : public llm_client {
public:
  explicit scripted_llm_client(std::vector<llm_response> responses)
      : responses_(std::move(responses)) {
  }

  llm_response complete(const llm_request& request) override {
    requests.push_back(request);
    if (responses_.empty()) {
      return { .content = "default" };
    }
    auto response = responses_.front();
    responses_.erase(responses_.begin());
    return response;
  }

  std::vector<llm_request> requests;

private:
  std::vector<llm_response> responses_;
};

class streaming_scripted_llm_client final : public llm_client {
public:
  bool supports_streaming() const noexcept override {
    return true;
  }

  llm_response complete(const llm_request& request) override {
    requests.push_back(request);
    return { .content = "stream-capable answer" };
  }

  std::vector<llm_request> requests;
};

class nonstandard_throwing_llm_client final : public llm_client {
public:
  llm_response complete(const llm_request&) override {
    throw 42;
  }
};

class streaming_tool_call_llm_client final : public llm_client {
public:
  bool supports_streaming() const noexcept override {
    return true;
  }

  llm_response complete(const llm_request& request) override {
    requests.push_back(request);
    return {
      .content = "draft",
      .tool_calls = {
        {
          .id = "call-1",
          .name = "echo_tool",
          .arguments_json = R"({"text":"from stream"})",
        },
      },
    };
  }

  llm_response complete_stream(const llm_request& request, const llm_stream_callbacks& callbacks,
    std::stop_token stop_token = {}) override {
    if (stop_token.stop_requested()) {
      return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
    }
    requests.push_back(request);
    llm_response response {
      .content = "draft",
      .reasoning_summary = "checking streamed tool",
      .tool_calls = {
        {
          .id = "call-1",
          .name = "echo_tool",
          .arguments_json = R"({"text":"from stream"})",
        },
      },
    };
    if (callbacks.on_event) {
      callbacks.on_event({
        .type = llm_stream_event_type::reasoning_delta,
        .reasoning_delta = "checking streamed tool",
      });
      callbacks.on_event({
        .type = llm_stream_event_type::content_delta,
        .content_delta = "draft",
      });
      callbacks.on_event({
        .type = llm_stream_event_type::tool_call_delta,
        .tool_call_delta =
          llm_tool_call_delta {
            .index = 0,
            .id = "call-1",
            .name_delta = "echo_",
            .arguments_delta = R"({"text")",
          },
      });
      callbacks.on_event({
        .type = llm_stream_event_type::tool_call_delta,
        .tool_call_delta =
          llm_tool_call_delta {
            .index = 0,
            .name_delta = "tool",
            .arguments_delta = R"(: "from stream"})",
          },
      });
      callbacks.on_event({
        .type = llm_stream_event_type::tool_call_done,
        .tool_call = response.tool_calls.front(),
      });
      callbacks.on_event({
        .type = llm_stream_event_type::reasoning_done,
        .reasoning_summary = response.reasoning_summary,
        .response = response,
      });
      callbacks.on_event({
        .type = llm_stream_event_type::done,
        .response = response,
      });
    }
    return response;
  }

  std::vector<llm_request> requests;
};

class cancellable_llm_client final : public llm_client {
public:
  llm_response complete(const llm_request& request) override {
    requests.push_back(request);
    return { .content = "late answer" };
  }

  llm_response complete(const llm_request& request, std::stop_token stop_token) override {
    requests.push_back(request);
    for (int index = 0; index < 100; ++index) {
      if (stop_token.stop_requested()) {
        return { .error_code = agent::make_error_code(agent::llm_error_code::cancelled) };
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return { .content = "late answer" };
  }

  std::vector<llm_request> requests;
};

struct echo_tool {
  static constexpr std::string_view description = "Echo text back to the caller.";

  std::string text;

  std::string invoke() const {
    return text;
  }
};

class recording_tool_provider {
public:
  std::vector<llm_tool> tools() const {
    return { {
      .name = "echo_tool",
      .description = "Echo text back to the caller.",
      .parameters_json_schema = R"({"type":"object"})",
    } };
  }

  llm_tool_result invoke(const std::string& name, const std::string& arguments_json) {
    calls.push_back({ .id = {}, .name = name, .arguments_json = arguments_json });
    return { .content = "unsafe output" };
  }

  std::vector<llm_tool_call> calls;
};

std::shared_ptr<routing::resource_aware_router> make_test_model_router() {
  auto router = std::make_shared<routing::resource_aware_router>();
  router->add({
    .model = "economy",
    .provider = "test",
    .context_window_tokens = 16'000,
    .max_output_tokens = 2'000,
    .input_cost_per_million_tokens = 100.0,
    .output_cost_per_million_tokens = 100.0,
    .quality_score = 0.55,
    .latency_score = 0.95,
    .capabilities = { .tools = true, .streaming = true },
  });
  router->add({
    .model = "premium",
    .provider = "test",
    .context_window_tokens = 16'000,
    .max_output_tokens = 2'000,
    .input_cost_per_million_tokens = 1'000.0,
    .output_cost_per_million_tokens = 1'000.0,
    .quality_score = 0.95,
    .latency_score = 0.55,
    .capabilities = { .tools = true, .streaming = true },
  });
  return router;
}

class scripted_reflector final : public reflection::reflector {
public:
  explicit scripted_reflector(std::vector<reflection::reflection_result> results)
      : results_(std::move(results)) {
  }

  reflection::reflection_result reflect(const reflection::reflection_request& request) override {
    requests.push_back(request);
    if (results_.empty()) {
      return reflection::reflection_result::pass();
    }
    auto result = results_.front();
    results_.erase(results_.begin());
    return result;
  }

  std::vector<reflection::reflection_request> requests;

private:
  std::vector<reflection::reflection_result> results_;
};

void simple_mode_runs_model_and_emits_events() {
  scripted_llm_client client({ { .content = "simple answer" } });
  std::vector<reasoning::reasoning_event_type> events;

  reasoning::reasoning_runner runner(
    client, [&](const reasoning::reasoning_event& event) { events.push_back(event.type); });

  auto result = runner.run({
    .input = "answer simply",
    .language = {
      .response_language = "zh-CN",
      .reasoning_language = "zh-CN",
    },
    .policy = {
      .mode = reasoning::reasoning_mode::simple,
    },
  });

  require(result.completed, "simple reasoning completes");
  require(result.content == "simple answer", "simple reasoning returns model content");
  require(client.requests.size() == 1, "simple reasoning calls model once");
  require(client.requests.front().language.response_language == "zh-CN",
    "reasoning runner should propagate response language to LLM requests");
  require(client.requests.front().language.reasoning_language == "zh-CN",
    "reasoning runner should propagate reasoning language to LLM requests");
  require(result.usage.model_calls == 1, "simple reasoning records model usage");
  require(!result.trace.empty(), "simple reasoning records a trace");
  require(result.trace.front().type == reasoning::reasoning_event_type::started,
    "simple reasoning trace starts with started");
  require(result.trace.back().type == reasoning::reasoning_event_type::completed,
    "simple reasoning trace ends with completed");
  require(
    events.front() == reasoning::reasoning_event_type::started, "simple reasoning emits started");
  require(events.back() == reasoning::reasoning_event_type::completed,
    "simple reasoning emits completed");
}

void async_run_reports_deltas_and_done() {
  scripted_llm_client client({ { .content = "async answer" } });
  reasoning::reasoning_runner runner(client);
  std::string streamed;
  std::atomic<bool> done { false };

  auto run = runner.run_async({
      .input = "answer asynchronously",
      .policy = {
        .mode = reasoning::reasoning_mode::simple,
      },
    },
    {
      .callbacks = {
        .on_delta = [&](std::string_view delta) {
          streamed += delta;
        },
        .on_done = [&](const reasoning::reasoning_result& result) {
          done = result.completed;
        },
      },
    });

  auto result = run.get();
  require(result.completed, "async reasoning completes");
  require(result.content == "async answer", "async reasoning returns content");
  require(streamed == "async answer", "async reasoning reports content deltas");
  require(done.load(), "async reasoning invokes done callback");
}

void async_run_can_be_cancelled() {
  cancellable_llm_client client;
  reasoning::reasoning_runner runner(client);
  std::atomic<bool> cancelled { false };

  auto run = runner.run_async({
      .input = "cancel this",
      .policy = {
        .mode = reasoning::reasoning_mode::simple,
      },
    },
    {
      .callbacks = {
        .on_cancelled = [&](const reasoning::reasoning_result& result) {
          cancelled = result.reasoning_error == reasoning::reasoning_error_code::cancelled;
        },
      },
    });

  run.request_stop();
  auto result = run.get();
  require(!result.completed, "cancelled async reasoning does not complete");
  require(result.reasoning_error == reasoning::reasoning_error_code::cancelled,
    "cancelled async reasoning reports reasoning cancellation");
  require(cancelled.load(), "cancelled async reasoning invokes cancellation callback");
}

void llm_errors_are_mapped_to_reasoning_errors() {
  scripted_llm_client client({
    {
      .error_code = agent::make_error_code(agent::llm_error_code::missing_api_key),
    },
  });
  reasoning::reasoning_runner runner(client);
  std::atomic<bool> saw_error { false };

  auto result = runner.run({
      .input = "needs a key",
      .policy = {
        .mode = reasoning::reasoning_mode::simple,
      },
    },
    {
      .callbacks = {
        .on_error = [&](const reasoning::reasoning_error& error) {
          saw_error = error.code == reasoning::reasoning_error_code::missing_api_key &&
                      error.underlying_error ==
                        agent::make_error_code(agent::llm_error_code::missing_api_key);
        },
      },
    });

  require(!result.completed, "llm error fails reasoning");
  require(result.reasoning_error == reasoning::reasoning_error_code::missing_api_key,
    "reasoning preserves missing api key as stable error");
  require(result.underlying_error == agent::make_error_code(agent::llm_error_code::missing_api_key),
    "reasoning preserves underlying llm error");
  require(saw_error.load(), "reasoning invokes error callback with mapped code");
}

void reflect_mode_preserves_model_error_details() {
  scripted_llm_client client({
    {
      .error_code = agent::make_error_code(agent::llm_error_code::authentication_failed),
    },
  });
  auto reflection_runner =
    std::make_shared<reflection::reflection_runner>(reflection::reflection_runner_options {
      .reflector =
        std::make_shared<scripted_reflector>(std::vector<reflection::reflection_result> {}),
    });
  reasoning::reasoning_runner runner({
    .client = &client,
    .reflection = reflection_runner,
  });

  auto result = runner.run({
    .input = "fail before reflection",
    .policy = {
      .mode = reasoning::reasoning_mode::reflect_and_retry,
    },
  });

  require(!result.completed, "reflect mode stops when the model fails");
  require(result.reasoning_error == reasoning::reasoning_error_code::authentication_failed,
    "reflect mode preserves model reasoning error");
  require(
    result.underlying_error == agent::make_error_code(agent::llm_error_code::authentication_failed),
    "reflect mode preserves underlying model error");
}

void policy_selector_and_trace_json_are_stable() {
  auto simple = reasoning::select_policy({
    .input = "What is this?",
  });
  require(simple.mode == reasoning::reasoning_mode::simple,
    "policy selector chooses simple for plain answers");

  auto planned = reasoning::select_policy({
    .input = "Create a multi-step workflow",
  });
  require(planned.mode == reasoning::reasoning_mode::plan_execute,
    "policy selector chooses planning for multi-step workflow");

  reasoning::reasoning_trace_record record {
    .sequence = 1,
    .type = reasoning::reasoning_event_type::completed,
    .mode = reasoning::reasoning_mode::simple,
    .message = "done",
  };
  auto json = reasoning::reasoning_trace_to_json({ record });
  require(json.is_array() && json.size() == 1, "trace json exports an array");
  require(json.at(0).at("type") == "completed", "trace json exports event type");
  require(json.at(0).at("mode") == "simple", "trace json exports reasoning mode");
}

void react_mode_uses_tool_provider() {
  scripted_llm_client client({
    {
      .tool_calls = {
        {
          .id = "call-1",
          .name = "echo_tool",
          .arguments_json = R"({"text":"from tool"})",
        },
      },
    },
    { .content = "final with tool" },
  });

  auto provider = std::make_shared<tool_provider<echo_tool>>();
  std::vector<reasoning::reasoning_event_type> events;
  auto runner = reasoning::reasoning_runner::with_tools(client,
    provider,
    {
      .observer = [&](const reasoning::reasoning_event& event) { events.push_back(event.type); },
    });

  auto result = runner.run({
    .input = "use a tool",
    .policy = {
      .mode = reasoning::reasoning_mode::react,
      .budget = {
        .max_tool_rounds = 2,
      },
    },
  });

  require(result.completed, "react reasoning completes");
  require(result.content == "final with tool", "react reasoning returns final content");
  require(client.requests.size() == 2, "react reasoning performs follow-up model call");
  require(result.usage.model_calls == 2, "react reasoning records each model call");
  require(result.usage.tool_calls == 1, "react reasoning records tool usage");
  require(std::find(events.begin(), events.end(), reasoning::reasoning_event_type::tool_started) !=
            events.end(),
    "react reasoning emits tool start");
  require(
    std::find(events.begin(), events.end(), reasoning::reasoning_event_type::tool_completed) !=
      events.end(),
    "react reasoning emits tool result");
}

void react_mode_maps_agent_stream_events_to_reasoning_events() {
  streaming_tool_call_llm_client client;
  auto provider = std::make_shared<tool_provider<echo_tool>>();

  std::vector<reasoning::reasoning_event_type> events;
  std::string streamed_reasoning;
  auto runner = reasoning::reasoning_runner::with_tools(client,
    provider,
    {
      .observer = [&](const reasoning::reasoning_event& event) { events.push_back(event.type); },
    });

  auto result = runner.run({
    .input = "use a streamed tool",
    .policy = {
      .mode = reasoning::reasoning_mode::react,
      .budget = {
        .max_tool_rounds = 0,
      },
      .enable_streaming = true,
    },
  },
  {
    .callbacks = {
      .on_reasoning_delta = [&](std::string_view delta) {
        streamed_reasoning += delta;
      },
    },
  });

  require(!result.completed,
    "streamed tool call with zero tool rounds should stop before tool execution");
  require(result.reasoning_summary == "checking streamed tool",
    "reasoning should preserve final provider reasoning summary");
  require(streamed_reasoning == "checking streamed tool",
    "reasoning callbacks should receive provider reasoning deltas");
  const auto has = [&](reasoning::reasoning_event_type type) {
    return std::find(events.begin(), events.end(), type) != events.end();
  };
  require(has(reasoning::reasoning_event_type::model_first_event),
    "reasoning should map agent model_first_event");
  require(has(reasoning::reasoning_event_type::content_delta),
    "reasoning should preserve streamed content deltas");
  require(has(reasoning::reasoning_event_type::reasoning_delta),
    "reasoning should preserve streamed reasoning deltas");
  require(has(reasoning::reasoning_event_type::reasoning_completed),
    "reasoning should preserve reasoning completion");
  require(has(reasoning::reasoning_event_type::tool_call_building),
    "reasoning should map streamed tool call deltas");
  require(has(reasoning::reasoning_event_type::tool_call_ready),
    "reasoning should map completed streamed tool calls");
  require(
    has(reasoning::reasoning_event_type::model_completed), "reasoning should map model completion");
}

void default_agentic_runner_wires_standard_capabilities() {
  scripted_llm_client client({ { .content = "default answer" } });
  auto provider = std::make_shared<tool_provider<echo_tool>>();
  auto runner = reasoning::make_default_agentic_runner(client, provider);

  auto simple = runner.run({
    .input = "answer simply",
    .policy = reasoning::select_policy(reasoning::reasoning_task_profile::simple_answer),
  });
  require(simple.completed, "default agentic runner handles simple mode");
  require(simple.content == "default answer", "default agentic runner returns simple content");

  auto reflected = runner.run({
    .input = "reflect",
    .policy = reasoning::select_policy(reasoning::reasoning_task_profile::high_confidence_answer),
  });
  require(reflected.completed, "default agentic runner includes reflection support");

  auto planned = runner.run({
    .input = "plan this",
    .policy = reasoning::select_policy(reasoning::reasoning_task_profile::plan_required),
  });
  require(planned.completed, "default agentic runner includes planning support");
}

void simple_mode_with_tool_provider_does_not_execute_tools() {
  scripted_llm_client client({
    {
      .tool_calls = {
        {
          .id = "call-1",
          .name = "echo_tool",
          .arguments_json = R"({"text":"from tool"})",
        },
      },
    },
    { .content = "should not be requested" },
  });

  auto provider = std::make_shared<tool_provider<echo_tool>>();
  auto runner = reasoning::reasoning_runner::with_tools(client, provider);

  auto result = runner.run({
    .input = "answer simply",
    .policy = {
      .mode = reasoning::reasoning_mode::simple,
    },
  });

  require(result.completed, "simple reasoning still completes with tool-shaped response");
  require(client.requests.size() == 1, "simple reasoning does not follow up with tools");
  require(result.usage.model_calls == 1, "simple reasoning with provider records one model call");
  require(result.usage.tool_calls == 0, "simple reasoning with provider records no tool usage");
}

void reflect_and_retry_retries_until_reflection_passes() {
  scripted_llm_client client({
    { .content = "bad answer" },
    { .content = "good answer" },
  });

  auto reflector = std::make_shared<scripted_reflector>(std::vector<reflection::reflection_result> {
    {
      .passed = false,
      .score = 0.5,
      .recommended_action = reflection::reflection_action::retry,
      .issues = {
        {
          .severity = reflection::reflection_severity::warning,
          .code = "thin",
          .message = "answer is thin",
          .suggestion = "add detail",
        },
      },
    },
    reflection::reflection_result::pass(),
  });
  auto reflection_runner =
    std::make_shared<reflection::reflection_runner>(reflection::reflection_runner_options {
      .reflector = reflector,
    });

  reasoning::reasoning_runner runner({
    .client = &client,
    .reflection = reflection_runner,
  });

  auto result = runner.run({
    .input = "make it good",
    .policy = {
      .mode = reasoning::reasoning_mode::reflect_and_retry,
      .budget = {
        .max_reflection_attempts = 2,
      },
    },
  });

  require(result.completed, "reflect-and-retry completes after passing reflection");
  require(result.content == "good answer", "reflect-and-retry returns passing output");
  require(result.reflections.size() == 2, "reflect-and-retry records reflection attempts");
  require(client.requests.size() == 2, "reflect-and-retry calls model twice");
  require(result.usage.model_calls == 2, "reflect-and-retry records model usage");
  require(result.usage.reflection_calls == 2, "reflect-and-retry records reflection usage");
  require(std::find_if(result.trace.begin(),
            result.trace.end(),
            [](const reasoning::reasoning_trace_record& record) {
              return record.type == reasoning::reasoning_event_type::model_started &&
                     record.mode == reasoning::reasoning_mode::reflect_and_retry;
            }) != result.trace.end(),
    "reflect-and-retry labels model events with the outer reasoning mode");
  require(
    client.requests[1].messages.back().content.find("Reflection feedback") != std::string::npos,
    "reflect-and-retry feeds critique into retry prompt");
}

void model_budget_stops_retry_before_second_model_call() {
  scripted_llm_client client({
    { .content = "bad answer" },
    { .content = "should not be requested" },
  });

  auto reflector = std::make_shared<scripted_reflector>(std::vector<reflection::reflection_result> {
    {
      .passed = false,
      .score = 0.5,
      .recommended_action = reflection::reflection_action::retry,
      .issues = {
        {
          .severity = reflection::reflection_severity::warning,
          .code = "thin",
          .message = "answer is thin",
        },
      },
    },
  });
  auto reflection_runner =
    std::make_shared<reflection::reflection_runner>(reflection::reflection_runner_options {
      .reflector = reflector,
    });

  reasoning::reasoning_runner runner({
    .client = &client,
    .reflection = reflection_runner,
  });

  auto result = runner.run({
    .input = "make it good",
    .policy = {
      .mode = reasoning::reasoning_mode::reflect_and_retry,
      .budget = {
        .max_model_calls = 1,
        .max_reflection_attempts = 2,
      },
    },
  });

  require(!result.completed, "model budget stops reflect-and-retry");
  require(result.error.find("model call budget") != std::string::npos,
    "model budget reports a clear error");
  require(client.requests.size() == 1, "model budget prevents second provider call");
  require(result.usage.model_calls == 1, "model budget records completed model calls");
  require(result.usage.reflection_calls == 1, "model budget records completed reflection calls");
  require(result.trace.back().type == reasoning::reasoning_event_type::failed,
    "model budget trace ends with failure");
}

void tool_budget_stops_tool_before_invocation() {
  scripted_llm_client client({
    {
      .tool_calls = {
        {
          .id = "call-1",
          .name = "echo_tool",
          .arguments_json = R"({"text":"from tool"})",
        },
      },
    },
  });

  auto provider = std::make_shared<tool_provider<echo_tool>>();
  auto runner = reasoning::reasoning_runner::with_tools(client, provider);

  auto result = runner.run({
    .input = "use a tool",
    .policy = {
      .mode = reasoning::reasoning_mode::react,
      .budget = {
        .max_tool_calls = 0,
        .max_tool_rounds = 2,
      },
    },
  });

  require(!result.completed, "tool budget stops react reasoning");
  require(result.error.find("tool call budget") != std::string::npos,
    "tool budget reports a clear error");
  require(result.usage.tool_calls == 0, "tool budget prevents tool invocation");
}

void tool_round_budget_maps_to_stable_reasoning_error() {
  scripted_llm_client client({
    {
      .content = "need tool",
      .tool_calls = {
        {
          .id = "call-1",
          .name = "echo_tool",
          .arguments_json = R"({"text":"from tool"})",
        },
      },
    },
    {
      .content = "need tool again",
      .tool_calls = {
        {
          .id = "call-2",
          .name = "echo_tool",
          .arguments_json = R"({"text":"again"})",
        },
      },
    },
  });

  auto provider = std::make_shared<tool_provider<echo_tool>>();
  auto runner = reasoning::reasoning_runner::with_tools(client, provider);
  std::atomic<bool> saw_error { false };

  auto result = runner.run({
      .input = "use tools until exhausted",
      .policy = {
        .mode = reasoning::reasoning_mode::react,
        .budget = {
          .max_model_calls = 4,
          .max_tool_calls = 4,
          .max_tool_rounds = 1,
        },
      },
    },
    {
      .callbacks = {
        .on_error = [&](const reasoning::reasoning_error& error) {
          saw_error = error.code ==
                        reasoning::reasoning_error_code::tool_round_budget_exceeded &&
                      error.underlying_error ==
                        agent::make_error_code(
                          agent::llm_error_code::agent_loop_budget_exceeded);
        },
      },
    });

  require(!result.completed, "tool round budget should fail reasoning");
  require(result.reasoning_error == reasoning::reasoning_error_code::tool_round_budget_exceeded,
    "reasoning should expose a stable tool-round budget error");
  require(result.underlying_error ==
            agent::make_error_code(agent::llm_error_code::agent_loop_budget_exceeded),
    "reasoning should preserve the underlying agent-loop budget error");
  require(result.error.find("tool round budget") != std::string::npos,
    "reasoning should expose a clear developer message");
  require(result.usage.tool_rounds == 1, "reasoning should record used tool rounds");
  require(result.usage.max_tool_rounds == 1, "reasoning should record max tool rounds");
  require(result.final_response.stop_reason == "tool_round_budget_exceeded",
    "reasoning should preserve the runtime stop reason");
  require(result.final_response.metadata.at("last_tool_call") == "echo_tool",
    "reasoning should preserve last tool call metadata");
  require(result.trace.back().metadata.at("stop_reason") == "tool_round_budget_exceeded",
    "terminal trace should include stop reason metadata");
  require(saw_error.load(), "reasoning should invoke on_error with stable code");

  const auto json = reasoning::reasoning_result_to_json(result);
  require(json.at("reasoning_error") == "tool_round_budget_exceeded",
    "reasoning JSON should export the stable error string");
  require(json.at("usage").at("tool_rounds") == 1, "reasoning JSON should export used tool rounds");
  require(
    json.at("usage").at("max_tool_rounds") == 1, "reasoning JSON should export max tool rounds");
}

void plan_execute_mode_delegates_to_planning() {
  auto planner = std::make_shared<planning::static_planner>(std::vector<planning::plan_step> {
    {
      .id = "first",
      .title = "First",
    },
    {
      .id = "second",
      .title = "Second",
      .depends_on = { "first" },
    },
  });

  std::vector<std::string> executed;
  auto executor = std::make_shared<planning::function_plan_executor>(
    [&](const planning::plan_step& step, const planning::plan_execution_context&) {
      executed.push_back(step.id);
      return planning::plan_step_result::completed("done-" + step.id);
    });

  reasoning::reasoning_runner runner({
    .planner = planner,
    .executor = executor,
  });

  auto result = runner.run({
    .input = "run the plan",
    .policy = {
      .mode = reasoning::reasoning_mode::plan_execute,
      .budget = {
        .max_steps = 4,
      },
    },
  });

  require(result.completed, "plan reasoning completes");
  require(result.content == "done-second", "plan reasoning returns plan final output");
  require(result.plan.has_value(), "plan reasoning stores plan result");
  require(executed.size() == 2, "plan reasoning executes both steps");
  require(result.usage.plan_steps == 2, "plan reasoning records plan step usage");
  require(
    executed[0] == "first" && executed[1] == "second", "plan reasoning respects dependencies");
}

void plan_execute_run_timeout_maps_to_planning_budget_error() {
  auto planner = std::make_shared<planning::static_planner>(std::vector<planning::plan_step> {
    { .id = "slow", .title = "Slow" },
  });
  auto executor = std::make_shared<planning::function_plan_executor>(
    [](const planning::plan_step&, const planning::plan_execution_context& context) {
      while (!context.cancellation_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return planning::plan_step_result::failed("cancelled");
    },
    planning::plan_executor_capabilities { .cooperative_cancellation = true });

  reasoning::reasoning_runner runner({
    .planner = planner,
    .executor = executor,
  });
  const auto result = runner.run({
    .input = "time-bound plan",
    .policy = {
      .mode = reasoning::reasoning_mode::plan_execute,
      .budget = {
        .max_steps = 2,
        .timeout = std::chrono::milliseconds(20),
      },
    },
  });

  require(!result.completed, "timed out plan reasoning does not complete");
  require(result.reasoning_error == reasoning::reasoning_error_code::planning_budget_exceeded,
    "plan run timeout maps to the stable planning budget error");
  require(result.plan.has_value() &&
            result.plan->stop_reason == planning::plan_run_stop_reason::run_timeout,
    "reasoning preserves the planning run timeout reason");
}

void input_guardrail_blocks_before_model_execution() {
  scripted_llm_client client({ { .content = "should not run" } });
  auto pipeline = std::make_shared<guardrails::guardrail_pipeline>();
  pipeline->add(std::make_shared<guardrails::text_guardrail>(guardrails::text_guardrail_options {
    .denied_terms = { "forbidden" },
  }));
  reasoning::reasoning_runner runner({
    .client = &client,
    .guardrail_pipeline = pipeline,
  });

  const auto result = runner.run({
    .input = "forbidden request",
    .policy = { .mode = reasoning::reasoning_mode::simple },
  });

  require(!result.completed, "input guardrail blocks reasoning");
  require(result.reasoning_error == reasoning::reasoning_error_code::input_guardrail_blocked,
    "input guardrail uses a stable reasoning error");
  require(client.requests.empty(), "blocked input never reaches the model");
  require(result.guardrail_runs.size() == 1 && !result.guardrail_runs.front().allowed(),
    "reasoning result preserves the input guardrail decision");
}

void output_guardrail_redacts_before_buffered_delivery() {
  scripted_llm_client client({ {
    .content = "answer token-123",
    .reasoning_summary = "summary token-123",
  } });
  auto pipeline = std::make_shared<guardrails::guardrail_pipeline>();
  pipeline->add(std::make_shared<guardrails::text_guardrail>(guardrails::text_guardrail_options {
    .redacted_terms = { "token-123" },
  }));
  agent::memory::memory_context memory;
  memory.set_scope({ .conversation_id = "redacted-output" });
  reasoning::reasoning_runner runner({
    .client = &client,
    .memory = &memory,
    .guardrail_pipeline = pipeline,
  });
  std::vector<std::string> deltas;
  std::vector<std::string> summaries;

  const auto result = runner.run(
    {
      .input = "safe request",
      .policy = { .mode = reasoning::reasoning_mode::simple },
    },
    {
      .callbacks = {
        .on_delta = [&](std::string_view delta) { deltas.emplace_back(delta); },
        .on_reasoning_done = [&](std::string_view summary) { summaries.emplace_back(summary); },
      },
    });

  require(result.completed, "redacted output remains successful");
  require(result.content == "answer [REDACTED]", "output guardrail returns sanitized content");
  require(result.final_response.content == result.content,
    "sanitized output remains consistent with the final response");
  require(result.reasoning_summary == "summary [REDACTED]" &&
            result.final_response.reasoning_summary == result.reasoning_summary,
    "reasoning summaries pass through the same output guardrails");
  require(deltas.size() == 1 && deltas.front() == "answer [REDACTED]",
    "guarded streaming only publishes sanitized output");
  require(summaries.size() == 1 && summaries.front() == "summary [REDACTED]",
    "guarded reasoning summaries are delivered only after sanitization");
  require(result.guardrail_runs.size() == 3 && result.guardrail_runs.back().modified(),
    "reasoning records input, output, and reasoning-summary guardrail runs");
  require(result.guardrail_runs.back().content.empty() &&
            !result.guardrail_runs.back().checks.front().result.replacement_content,
    "reasoning guardrail diagnostics do not retain guarded content");
  const auto remembered = memory.list();
  require(
    std::any_of(remembered.begin(),
      remembered.end(),
      [](const auto& record) { return record.content == "answer [REDACTED]"; }) &&
      std::none_of(remembered.begin(),
        remembered.end(),
        [](const auto& record) { return record.content.find("token-123") != std::string::npos; }),
    "memory receives only the sanitized final assistant output");
}

void output_guardrail_denial_does_not_leak_candidate_output() {
  scripted_llm_client client({ { .content = "unsafe candidate" } });
  auto pipeline = std::make_shared<guardrails::guardrail_pipeline>();
  pipeline->add(std::make_shared<guardrails::text_guardrail>(guardrails::text_guardrail_options {
    .denied_terms = { "unsafe" },
  }));
  agent::memory::memory_context memory;
  memory.set_scope({ .conversation_id = "denied-output" });
  reasoning::reasoning_runner runner({
    .client = &client,
    .memory = &memory,
    .guardrail_pipeline = pipeline,
  });

  const auto result = runner.run({
    .input = "safe request",
    .policy = { .mode = reasoning::reasoning_mode::simple },
  });

  require(!result.completed, "denied output fails reasoning");
  require(result.reasoning_error == reasoning::reasoning_error_code::output_guardrail_blocked,
    "output guardrail uses a stable reasoning error");
  require(result.content.empty() && result.final_response.content.empty(),
    "blocked candidate output is removed from public result fields");
  require(result.steps.empty() || result.steps.back().output.empty(),
    "blocked candidate output is removed from reasoning steps");
  require(result.guardrail_runs.back().content.empty(),
    "blocked candidate output is not retained in guardrail diagnostics");
  const auto remembered = memory.list();
  require(std::none_of(remembered.begin(),
            remembered.end(),
            [](const auto& record) { return record.content == "unsafe candidate"; }),
    "blocked candidate output is not persisted to memory");
}

void failed_provider_payload_is_guarded() {
  scripted_llm_client client({ {
    .content = "unsafe provider error",
    .error_code = agent::make_error_code(agent::llm_error_code::transport_error),
    .metadata = { { "provider_detail", "unsafe metadata" } },
  } });
  auto pipeline = std::make_shared<guardrails::guardrail_pipeline>();
  pipeline->add(std::make_shared<guardrails::text_guardrail>(
    guardrails::text_guardrail_options { .redacted_terms = { "unsafe" } }));
  reasoning::reasoning_runner runner({
    .client = &client,
    .guardrail_pipeline = pipeline,
  });

  const auto result = runner.run({
    .input = "safe request",
    .policy = { .mode = reasoning::reasoning_mode::simple },
  });

  require(!result.completed, "a provider failure remains a failed reasoning run");
  require(
    result.content == "[REDACTED] provider error" && result.error == "[REDACTED] provider error",
    "provider failure content and public error both pass through output guardrails");
  require(result.final_response.metadata.empty() &&
            (result.steps.empty() || result.steps.back().metadata.empty()),
    "modification removes nested provider metadata that may retain raw payloads");
}

void denied_plan_output_removes_nested_payloads() {
  auto planner = std::make_shared<planning::static_planner>(std::vector<planning::plan_step> {
    { .id = "produce", .title = "Produce" },
  });
  auto executor = std::make_shared<planning::function_plan_executor>(
    [](const planning::plan_step&, const planning::plan_execution_context&) {
      planning::plan_step_result result = planning::plan_step_result::completed("unsafe output");
      result.output_json = { { "raw", "unsafe json" } };
      result.artifacts["unsafe-artifact"] = "unsafe artifact";
      result.metadata["raw"] = "unsafe metadata";
      return result;
    });
  auto pipeline = std::make_shared<guardrails::guardrail_pipeline>();
  pipeline->add(std::make_shared<guardrails::function_guardrail>(
    "deny-output", [](const guardrails::guardrail_request& request) {
      if (request.stage != guardrails::guardrail_stage::output) {
        return guardrails::guardrail_result::allow();
      }
      auto denied = guardrails::guardrail_result::deny({
        .severity = guardrails::guardrail_severity::critical,
        .code = "unsafe_output",
        .message = "unsafe output was observed",
        .evidence = request.content,
        .remediation = "remove unsafe output",
        .metadata = { { "raw", request.content } },
      });
      denied.metadata["raw"] = request.content;
      return denied;
    }));
  reasoning::reasoning_runner runner({
    .planner = planner,
    .executor = executor,
    .guardrail_pipeline = pipeline,
  });

  const auto result = runner.run({
    .input = "safe plan",
    .policy = {
      .mode = reasoning::reasoning_mode::plan_execute,
      .budget = { .max_steps = 2 },
    },
  });

  require(!result.completed &&
            result.reasoning_error == reasoning::reasoning_error_code::output_guardrail_blocked,
    "denied plan output reports the output guardrail error");
  require(result.error == "reasoning blocked by guardrail",
    "guardrail denial returns a fixed public error rather than policy details");
  require(result.plan.has_value(), "the sanitized plan structure remains inspectable");
  require(result.plan->final_output.empty() && result.plan->error.empty() &&
            result.plan->value.artifacts.empty() && result.plan->value.metadata.empty(),
    "blocked plans remove final output, errors, artifacts, and metadata");
  const auto& step = result.plan->value.steps.front();
  require(step.output.empty() && step.output_json.is_null() && step.error.empty() &&
            step.metadata.empty() && step.produced_artifacts.empty(),
    "blocked plans remove every step output payload");
  for (const auto& trace : result.trace) {
    require(trace.message.find("unsafe") == std::string::npos &&
              trace.delta.find("unsafe") == std::string::npos &&
              trace.error.find("unsafe") == std::string::npos && trace.metadata.empty(),
      "blocked output does not survive in reasoning trace payloads");
  }
  const auto& diagnostics = result.guardrail_runs.back();
  require(diagnostics.metadata.empty() && diagnostics.issues.front().message.empty() &&
            diagnostics.issues.front().evidence.empty() &&
            diagnostics.issues.front().remediation.empty() &&
            diagnostics.issues.front().metadata.empty() &&
            diagnostics.checks.front().result.metadata.empty(),
    "default guardrail diagnostics retain codes but remove sensitive details");
}

void denied_reflected_output_removes_reflection_payloads() {
  scripted_llm_client client({ { .content = "unsafe candidate" } });
  auto reflector = std::make_shared<scripted_reflector>(
    std::vector<reflection::reflection_result> { reflection::reflection_result::pass() });
  auto reflection_runner = std::make_shared<reflection::reflection_runner>(
    reflection::reflection_runner_options { .reflector = reflector });
  auto pipeline = std::make_shared<guardrails::guardrail_pipeline>();
  pipeline->add(std::make_shared<guardrails::text_guardrail>(
    guardrails::text_guardrail_options { .denied_terms = { "unsafe" } }));
  reasoning::reasoning_runner runner({
    .client = &client,
    .reflection = reflection_runner,
    .guardrail_pipeline = pipeline,
  });

  const auto result = runner.run({
    .input = "safe request",
    .policy = {
      .mode = reasoning::reasoning_mode::reflect_and_retry,
      .budget = { .max_reflection_attempts = 1 },
    },
  });
  require(!result.completed && result.reflections.empty(),
    "blocked reflected output removes reflection records containing the candidate");
}

void tool_guardrails_modify_model_context() {
  scripted_llm_client client({
    {
      .tool_calls = { {
        .id = "call-1",
        .name = "echo_tool",
        .arguments_json = R"({"text":"unsafe input"})",
      } },
    },
    { .content = "final answer" },
  });
  auto pipeline = std::make_shared<guardrails::guardrail_pipeline>();
  pipeline->add(std::make_shared<guardrails::function_guardrail>(
    "tool-sanitizer", [](const guardrails::guardrail_request& request) {
      if (request.stage == guardrails::guardrail_stage::tool_input) {
        return guardrails::guardrail_result::modify(R"({"text":"safe input"})");
      }
      if (request.stage == guardrails::guardrail_stage::tool_output) {
        return guardrails::guardrail_result::modify("safe output");
      }
      return guardrails::guardrail_result::allow();
    }));
  auto provider = std::make_shared<recording_tool_provider>();
  auto runner = reasoning::reasoning_runner::with_tools(client,
    provider,
    {
      .guardrail_pipeline = pipeline,
    });

  const auto result = runner.run({
    .input = "use a tool",
    .policy = {
      .mode = reasoning::reasoning_mode::react,
      .budget = { .max_tool_rounds = 2 },
    },
  });

  require(result.completed && provider->calls.size() == 1,
    "modified tool calls still execute exactly once");
  require(provider->calls.front().arguments_json == R"({"text":"safe input"})",
    "the tool implementation receives sanitized arguments");
  require(client.requests.size() == 2, "tool execution triggers a follow-up model call");
  const auto& messages = client.requests.back().messages;
  const auto assistant =
    std::find_if(messages.begin(), messages.end(), [](const chat_message& value) {
      return value.role == "assistant" && !value.tool_calls.empty();
    });
  const auto tool = std::find_if(messages.begin(), messages.end(), [](const chat_message& value) {
    return value.role == "tool";
  });
  require(assistant != messages.end() &&
            assistant->tool_calls.front().arguments_json == R"({"text":"safe input"})",
    "sanitized tool arguments replace the original assistant tool call in model context");
  require(tool != messages.end() && tool->content == "safe output",
    "sanitized tool results are the only result content sent to the next model call");
  require(result.guardrail_runs.size() == 4,
    "reasoning records input, tool input, tool output, and final output checks");
}

void tool_guardrail_denials_stop_execution_safely() {
  scripted_llm_client input_client({ {
    .content = "sensitive draft",
    .tool_calls = { {
      .id = "call-1",
      .name = "echo_tool",
      .arguments_json = R"({"text":"blocked"})",
    } },
  } });
  auto input_pipeline = std::make_shared<guardrails::guardrail_pipeline>();
  input_pipeline->add(std::make_shared<guardrails::function_guardrail>(
    "block-tool-input", [](const guardrails::guardrail_request& request) {
      return request.stage == guardrails::guardrail_stage::tool_input
               ? guardrails::guardrail_result::deny({
                   .code = "blocked_tool_input",
                   .message = "blocked raw arguments",
                 })
               : guardrails::guardrail_result::allow();
    }));
  auto provider = std::make_shared<recording_tool_provider>();
  agent::memory::memory_context memory;
  memory.set_scope({ .conversation_id = "denied-tool-input" });
  auto input_runner = reasoning::reasoning_runner::with_tools(input_client,
    provider,
    {
      .memory = &memory,
      .guardrail_pipeline = input_pipeline,
    });
  const auto input_result = input_runner.run({
    .input = "use a tool",
    .policy = {
      .mode = reasoning::reasoning_mode::react,
      .budget = { .max_tool_rounds = 1 },
    },
  });
  require(
    !input_result.completed &&
      input_result.reasoning_error == reasoning::reasoning_error_code::tool_input_guardrail_blocked,
    "tool input denial maps to its stable reasoning error");
  require(provider->calls.empty() && input_client.requests.size() == 1,
    "denied tool input is neither invoked nor followed by another model call");
  require(input_result.error == "reasoning blocked by guardrail",
    "tool input denial does not expose guardrail issue details");
  const auto remembered = memory.list();
  require(std::none_of(remembered.begin(),
            remembered.end(),
            [](const auto& record) { return record.content == "sensitive draft"; }),
    "a model response rejected at the tool boundary is not persisted to memory");

  scripted_llm_client output_client({ {
    .content = "sensitive tool draft",
    .tool_calls = { {
      .id = "call-2",
      .name = "echo_tool",
      .arguments_json = R"({"text":"blocked result"})",
    } },
  } });
  auto output_pipeline = std::make_shared<guardrails::guardrail_pipeline>();
  output_pipeline->add(std::make_shared<guardrails::function_guardrail>(
    "block-tool-output", [](const guardrails::guardrail_request& request) {
      return request.stage == guardrails::guardrail_stage::tool_output
               ? guardrails::guardrail_result::deny({
                   .code = "blocked_tool_output",
                   .message = "blocked raw result",
                 })
               : guardrails::guardrail_result::allow();
    }));
  agent::memory::memory_context output_memory;
  output_memory.set_scope({ .conversation_id = "denied-tool-output" });
  auto output_runner = reasoning::reasoning_runner::with_tools(output_client,
    provider,
    {
      .memory = &output_memory,
      .guardrail_pipeline = output_pipeline,
    });
  const auto output_result = output_runner.run({
    .input = "use a tool",
    .policy = {
      .mode = reasoning::reasoning_mode::react,
      .budget = { .max_tool_rounds = 1 },
    },
  });
  require(
    !output_result.completed && output_result.reasoning_error ==
                                  reasoning::reasoning_error_code::tool_output_guardrail_blocked,
    "tool output denial maps to its stable reasoning error");
  require(provider->calls.size() == 1 && output_client.requests.size() == 1,
    "denied tool output stops before a follow-up model call");
  require(output_result.content.empty() && output_result.final_response.tool_calls.empty(),
    "denied tool output removes the original model tool call payload");
  const auto output_records = output_memory.list();
  require(std::none_of(output_records.begin(),
            output_records.end(),
            [](const auto& record) { return record.content == "sensitive tool draft"; }),
    "a tool-output rejection prevents the uncommitted assistant draft from reaching memory");
}

void guarded_buffering_hides_raw_tool_call_stream_events() {
  streaming_tool_call_llm_client client;
  auto provider = std::make_shared<tool_provider<echo_tool>>();
  auto pipeline = std::make_shared<guardrails::guardrail_pipeline>();
  pipeline->add(std::make_shared<guardrails::function_guardrail>("allow",
    [](const guardrails::guardrail_request&) { return guardrails::guardrail_result::allow(); }));
  std::vector<reasoning::reasoning_event_type> events;
  auto runner = reasoning::reasoning_runner::with_tools(client,
    provider,
    {
      .observer = [&](const reasoning::reasoning_event& event) { events.push_back(event.type); },
      .guardrail_pipeline = pipeline,
    });

  (void)runner.run({
    .input = "stream a tool",
    .policy = {
      .mode = reasoning::reasoning_mode::react,
      .budget = { .max_tool_rounds = 0 },
      .enable_streaming = true,
    },
  });

  require(
    std::find(events.begin(), events.end(), reasoning::reasoning_event_type::tool_call_building) ==
      events.end(),
    "guarded buffering suppresses unchecked tool argument deltas");
  require(
    std::find(events.begin(), events.end(), reasoning::reasoning_event_type::tool_call_ready) ==
      events.end(),
    "guarded buffering suppresses unchecked completed tool calls");
}

void resource_routing_switches_models_as_budget_changes() {
  scripted_llm_client client({
    {
      .usage = { .prompt_tokens = 100, .completion_tokens = 100, .total_tokens = 200 },
      .tool_calls = { {
        .id = "call-1",
        .name = "echo_tool",
        .arguments_json = R"({"text":"value"})",
      } },
    },
    {
      .content = "budget-aware answer",
      .usage = { .prompt_tokens = 100, .completion_tokens = 100, .total_tokens = 200 },
    },
  });
  auto provider = std::make_shared<recording_tool_provider>();
  auto runner = reasoning::reasoning_runner::with_tools(client,
    provider,
    {
      .model_router = make_test_model_router(),
      .token_estimator = [](const llm_request&) { return std::size_t { 100 }; },
    });

  const auto result = runner.run({
    .input = "use the best affordable model",
    .model = "premium",
    .policy = {
      .mode = reasoning::reasoning_mode::react,
      .budget = {
        .max_tool_rounds = 2,
        .max_completion_tokens = 200,
        .max_cost_usd = 0.25,
        .estimated_output_tokens_per_call = 100,
      },
    },
    .model_routing = {
      .strategy = routing::model_selection_strategy::highest_quality,
    },
  });

  require(result.completed && result.content == "budget-aware answer",
    "resource-aware reasoning completes within its cost budget");
  require(client.requests.size() == 2 && client.requests[0].model == "premium" &&
            client.requests[1].model == "economy",
    "each model round is rerouted using the remaining cost budget");
  require(
    client.requests[0].max_output_tokens == 200 && client.requests[1].max_output_tokens == 100,
    "reasoning propagates the remaining output token budget to each provider request");
  require(result.model_routes.size() == 2 && result.model_routes[0].selected_model == "premium" &&
            result.model_routes[1].selected_model == "economy",
    "reasoning exposes every model routing decision");
  require(result.usage.prompt_tokens == 200 && result.usage.completion_tokens == 200 &&
            result.usage.total_tokens == 400,
    "reasoning aggregates token usage across model rounds");
  require(std::abs(result.usage.cost_usd - 0.22) < 1e-9,
    "reasoning accounts actual token cost using the selected model profile");
  require(std::count_if(result.trace.begin(),
            result.trace.end(),
            [](const auto& event) {
              return event.type == reasoning::reasoning_event_type::model_routed;
            }) == 2,
    "model routing decisions are represented in the reasoning trace");
}

void resource_routing_requires_streaming_for_streamed_calls() {
  streaming_scripted_llm_client client;
  auto router = std::make_shared<routing::resource_aware_router>();
  router->add({
    .model = "non-streaming",
    .input_cost_per_million_tokens = 0.0,
    .output_cost_per_million_tokens = 0.0,
    .quality_score = 1.0,
    .latency_score = 1.0,
  });
  router->add({
    .model = "streaming",
    .input_cost_per_million_tokens = 0.0,
    .output_cost_per_million_tokens = 0.0,
    .quality_score = 0.8,
    .latency_score = 0.8,
    .capabilities = { .streaming = true },
  });
  reasoning::reasoning_runner runner({
    .client = &client,
    .model_router = router,
  });

  const auto result = runner.run({
    .input = "stream this answer",
    .policy = {
      .mode = reasoning::reasoning_mode::simple,
      .enable_streaming = true,
    },
    .model_routing = {
      .strategy = routing::model_selection_strategy::highest_quality,
    },
  });

  require(
    result.completed && client.requests.size() == 1 && client.requests.front().model == "streaming",
    "streamed provider calls only route to models that declare streaming support");
}

void resource_budgets_fail_before_or_after_model_calls() {
  scripted_llm_client estimated_client({ { .content = "estimated usage" } });
  reasoning::reasoning_runner estimated_runner({
    .client = &estimated_client,
    .model_router = make_test_model_router(),
    .token_estimator = [](const llm_request&) { return std::size_t { 25 }; },
  });
  const auto estimated = estimated_runner.run({
    .input = "no provider usage",
    .policy = {
      .mode = reasoning::reasoning_mode::simple,
      .budget = {
        .max_cost_usd = 0.1,
        .estimated_output_tokens_per_call = 50,
      },
    },
    .model_routing = { .strategy = routing::model_selection_strategy::lowest_cost },
  });
  require(estimated.completed && estimated.usage.estimated_token_calls == 1 &&
            estimated.usage.prompt_tokens == 25 && estimated.usage.completion_tokens == 50 &&
            estimated.usage.total_tokens == 75,
    "routing estimates are used for accounting when the provider omits token usage");
  require(std::abs(estimated.usage.cost_usd - 0.0075) < 1e-9,
    "estimated token usage produces conservative cost accounting");

  scripted_llm_client preflight_client({ { .content = "should not run" } });
  reasoning::reasoning_runner preflight_runner({
    .client = &preflight_client,
    .token_estimator = [](const llm_request&) { return std::size_t { 100 }; },
  });
  const auto preflight = preflight_runner.run({
    .input = "too large",
    .policy = {
      .mode = reasoning::reasoning_mode::simple,
      .budget = { .max_total_tokens = 50 },
    },
  });
  require(!preflight.completed &&
            preflight.reasoning_error == reasoning::reasoning_error_code::token_budget_exceeded &&
            preflight_client.requests.empty(),
    "estimated token budgets can reject a request before provider execution");

  scripted_llm_client actual_client({ {
    .content = "oversized answer",
    .usage = { .prompt_tokens = 20, .completion_tokens = 40, .total_tokens = 60 },
  } });
  reasoning::reasoning_runner actual_runner({
    .client = &actual_client,
    .token_estimator = [](const llm_request&) { return std::size_t { 5 }; },
  });
  const auto actual = actual_runner.run({
    .input = "small estimate",
    .policy = {
      .mode = reasoning::reasoning_mode::simple,
      .budget = {
        .max_total_tokens = 30,
        .estimated_output_tokens_per_call = 10,
      },
    },
  });
  require(!actual.completed &&
            actual.reasoning_error == reasoning::reasoning_error_code::token_budget_exceeded &&
            actual.usage.total_tokens == 60,
    "actual provider usage enforces token budgets after a call completes");

  scripted_llm_client inconsistent_client({ {
    .content = "underreported total",
    .usage = { .prompt_tokens = 10, .completion_tokens = 10, .total_tokens = 1 },
  } });
  reasoning::reasoning_runner inconsistent_runner({
    .client = &inconsistent_client,
    .token_estimator = [](const llm_request&) { return std::size_t { 1 }; },
  });
  const auto inconsistent = inconsistent_runner.run({
    .input = "provider total is inconsistent",
    .policy = {
      .mode = reasoning::reasoning_mode::simple,
      .budget = {
        .max_total_tokens = 15,
        .estimated_output_tokens_per_call = 1,
      },
    },
  });
  require(
    !inconsistent.completed &&
      inconsistent.reasoning_error == reasoning::reasoning_error_code::token_budget_exceeded &&
      inconsistent.usage.total_tokens == 20,
    "provider totals cannot underreport prompt plus completion usage");

  scripted_llm_client cost_client({ { .content = "should not run" } });
  reasoning::reasoning_runner cost_runner({ .client = &cost_client });
  const auto cost = cost_runner.run({
    .input = "cost constrained",
    .policy = {
      .mode = reasoning::reasoning_mode::simple,
      .budget = { .max_cost_usd = 0.01 },
    },
  });
  require(!cost.completed &&
            cost.reasoning_error == reasoning::reasoning_error_code::model_routing_failed &&
            cost_client.requests.empty(),
    "cost budgets require explicit model pricing instead of silently guessing");

  scripted_llm_client unaffordable_client({ { .content = "should not run" } });
  reasoning::reasoning_runner unaffordable_runner({
    .client = &unaffordable_client,
    .model_router = make_test_model_router(),
    .token_estimator = [](const llm_request&) { return std::size_t { 100 }; },
  });
  const auto unaffordable = unaffordable_runner.run({
    .input = "unaffordable",
    .policy = {
      .mode = reasoning::reasoning_mode::simple,
      .budget = {
        .max_cost_usd = 0.001,
        .estimated_output_tokens_per_call = 100,
      },
    },
  });
  require(!unaffordable.completed &&
            unaffordable.reasoning_error == reasoning::reasoning_error_code::cost_budget_exceeded &&
            unaffordable_client.requests.empty() && unaffordable.model_routes.size() == 1,
    "routing fails before execution when every eligible model exceeds remaining cost");
  require(std::any_of(unaffordable.trace.begin(),
            unaffordable.trace.end(),
            [](const auto& event) {
              return event.type == reasoning::reasoning_event_type::model_route_failed;
            }),
    "failed routing decisions are represented in the reasoning trace");
}

void nonstandard_provider_exceptions_are_contained() {
  nonstandard_throwing_llm_client client;
  reasoning::reasoning_runner runner(client);
  const auto result = runner.run({
    .input = "contain provider failure",
    .policy = { .mode = reasoning::reasoning_mode::simple },
  });
  require(!result.completed && result.reasoning_error == reasoning::reasoning_error_code::unknown &&
            !result.error.empty(),
    "reasoning must translate non-standard provider exceptions into a stable result");
}

void run(const char* name, void (*test)()) {
  test();
  println("[PASS] {}", name);
}

} // namespace

int main() {
  try {
    run("simple mode runs model and emits events", simple_mode_runs_model_and_emits_events);
    run("async run reports deltas and done", async_run_reports_deltas_and_done);
    run("async run can be cancelled", async_run_can_be_cancelled);
    run("llm errors are mapped to reasoning errors", llm_errors_are_mapped_to_reasoning_errors);
    run("reflect mode preserves model error details", reflect_mode_preserves_model_error_details);
    run("policy selector and trace json are stable", policy_selector_and_trace_json_are_stable);
    run("react mode uses tool provider", react_mode_uses_tool_provider);
    run("react mode maps agent stream events to reasoning events",
      react_mode_maps_agent_stream_events_to_reasoning_events);
    run("default agentic runner wires standard capabilities",
      default_agentic_runner_wires_standard_capabilities);
    run("simple mode with tool provider does not execute tools",
      simple_mode_with_tool_provider_does_not_execute_tools);
    run("reflect and retry retries until reflection passes",
      reflect_and_retry_retries_until_reflection_passes);
    run("model budget stops retry before second model call",
      model_budget_stops_retry_before_second_model_call);
    run("tool budget stops tool before invocation", tool_budget_stops_tool_before_invocation);
    run("tool round budget maps to stable reasoning error",
      tool_round_budget_maps_to_stable_reasoning_error);
    run("plan execute mode delegates to planning", plan_execute_mode_delegates_to_planning);
    run("plan execute run timeout maps to planning budget error",
      plan_execute_run_timeout_maps_to_planning_budget_error);
    run("input guardrail blocks before model execution",
      input_guardrail_blocks_before_model_execution);
    run("output guardrail redacts before buffered delivery",
      output_guardrail_redacts_before_buffered_delivery);
    run("output guardrail denial does not leak candidate output",
      output_guardrail_denial_does_not_leak_candidate_output);
    run("failed provider payload is guarded", failed_provider_payload_is_guarded);
    run("denied plan output removes nested payloads", denied_plan_output_removes_nested_payloads);
    run("denied reflected output removes reflection payloads",
      denied_reflected_output_removes_reflection_payloads);
    run("tool guardrails modify model context", tool_guardrails_modify_model_context);
    run(
      "tool guardrail denials stop execution safely", tool_guardrail_denials_stop_execution_safely);
    run("guarded buffering hides raw tool call stream events",
      guarded_buffering_hides_raw_tool_call_stream_events);
    run("resource routing switches models as budget changes",
      resource_routing_switches_models_as_budget_changes);
    run("resource routing requires streaming for streamed calls",
      resource_routing_requires_streaming_for_streamed_calls);
    run("resource budgets fail before or after model calls",
      resource_budgets_fail_before_or_after_model_calls);
    run("non-standard provider exceptions are contained",
      nonstandard_provider_exceptions_are_contained);
  }
  catch (const std::exception& ex) {
    println("[FAIL] {}", ex.what());
    return 1;
  }

  return 0;
}
