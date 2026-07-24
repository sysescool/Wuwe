#include <atomic>
#include <chrono>
#include <memory>
#include <filesystem>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <wuwe/agent/planning/planning.hpp>
#include <wuwe/agent/reflection/reflection.hpp>

namespace {

using namespace wuwe;
using namespace wuwe::agent::planning;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct echo_tool_provider {
  std::vector<llm_tool> tools() const {
    return {
      {
        .name = "echo",
        .description = "Echo arguments.",
        .parameters_json_schema = "{}",
      },
      {
        .name = "fail_once",
        .description = "Fail on the first call.",
        .parameters_json_schema = "{}",
      },
    };
  }

  llm_tool_result invoke(const std::string& name, const std::string& arguments_json) {
    if (name == "echo") {
      return { .content = arguments_json };
    }
    if (name == "fail_once") {
      ++fail_once_calls;
      if (fail_once_calls == 1) {
        return {
          .content = "temporary failure",
          .error_code = std::make_error_code(std::errc::resource_unavailable_try_again),
        };
      }
      return { .content = "recovered" };
    }
    return {
      .content = "missing",
      .error_code = std::make_error_code(std::errc::function_not_supported),
    };
  }

  int fail_once_calls {};
};

struct stop_aware_tool_provider {
  std::vector<llm_tool> tools() const {
    return {
      {
        .name = "stop_aware",
        .description = "Record stop token availability.",
        .parameters_json_schema = "{}",
      },
    };
  }

  llm_tool_result invoke(
    const std::string&,
    const std::string& arguments_json,
    std::stop_token stop_token) {
    saw_stop_possible = saw_stop_possible || stop_token.stop_possible();
    return { .content = arguments_json };
  }

  bool saw_stop_possible {};
};

class revising_planner final : public planner {
public:
  plan create_plan(const planning_request& request) override {
    return {
      .id = "needs-revision",
      .goal = request.goal,
      .steps = {
        {
          .id = "missing-tool",
          .title = "Missing tool",
          .assigned_tool = "missing",
          .input = "{}",
        },
      },
    };
  }

  plan revise_plan(const plan& current, const planning_observation& observation) override {
    revised = true;
    require(observation.status == plan_step_status::blocked, "observation preserves blocked status");
    return {
      .id = current.id,
      .goal = current.goal,
      .steps = {
        {
          .id = "replacement",
          .title = "Replacement",
          .assigned_tool = "echo",
          .input = "{\"ok\":true}",
        },
      },
    };
  }

  bool revised {};
};

class reflection_replanning_planner final : public planner {
public:
  plan create_plan(const planning_request& request) override {
    return {
      .id = "reflection-replan",
      .goal = request.goal,
      .steps = {
        {
          .id = "weak-answer",
          .title = "Weak answer",
        },
      },
    };
  }

  plan revise_plan(const plan& current, const planning_observation& observation) override {
    revised = true;
    require(observation.metadata.at("reflection_action") == "replan",
      "reflection observation requests replanning");
    return {
      .id = current.id,
      .goal = current.goal,
      .steps = {
        {
          .id = "replacement",
          .title = "Replacement",
        },
      },
    };
  }

  bool revised {};
};

class fixed_reflector final : public agent::reflection::reflector {
public:
  explicit fixed_reflector(
    agent::reflection::reflection_result result,
    bool pass_replacement = false)
      : result_(std::move(result)), pass_replacement_(pass_replacement) {
  }

  agent::reflection::reflection_result reflect(
    const agent::reflection::reflection_request& request) override {
    last_request = request;
    if (pass_replacement_ && request.original_input.find("replacement") != std::string::npos) {
      return agent::reflection::reflection_result::pass();
    }
    return result_;
  }

  agent::reflection::reflection_result result_;
  agent::reflection::reflection_request last_request;
  bool pass_replacement_ {};
};

class fake_llm_client final : public llm_client {
public:
  explicit fake_llm_client(std::string content) : content_(std::move(content)) {
  }

  llm_response complete(const llm_request& request) override {
    last_request = request;
    return { .content = content_ };
  }

  std::string content_;
  llm_request last_request;
};

void require_throws(auto&& callback, const std::string& expected_message) {
  try {
    callback();
  }
  catch (const std::exception& ex) {
    require(std::string(ex.what()).find(expected_message) != std::string::npos,
      std::string("exception message contains ") + expected_message + ": " + ex.what());
    return;
  }
  throw std::runtime_error("expected exception containing: " + expected_message);
}

void static_plan_respects_dependencies() {
  std::vector<std::string> executed;
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
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

  auto executor = std::make_shared<function_plan_executor>(
    [&](const plan_step& step, const plan_execution_context&) {
      executed.push_back(step.id);
      return plan_step_result::completed("done-" + step.id);
    });

  plan_runner runner({
    .planner = planner,
    .executor = executor,
  });

  const auto result = runner.run({ .goal = "run two steps" });
  require(result.completed, "dependent static plan completes");
  require(executed.size() == 2, "two steps executed");
  require(executed[0] == "first", "first dependency executed first");
  require(executed[1] == "second", "dependent step executed second");
  require(result.final_output == "done-second", "final output comes from last completed step");
}

void tool_executor_invokes_provider() {
  auto provider = std::make_shared<echo_tool_provider>();
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    {
      .id = "echo-step",
      .title = "Echo",
      .assigned_tool = "echo",
      .input = "{\"message\":\"hello\"}",
    },
  });
  auto executor = std::make_shared<tool_plan_executor>(provider);
  require(!executor->capabilities(plan_step {}).cooperative_cancellation,
    "simple tool provider does not claim cooperative cancellation");

  plan_runner runner({
    .planner = planner,
    .executor = executor,
  });

  const auto result = runner.run({ .goal = "call echo" });
  require(result.completed, "tool plan completes");
  require(result.final_output == "{\"message\":\"hello\"}", "tool output is surfaced");
}

void tool_executor_forwards_stop_token_to_provider() {
  auto provider = std::make_shared<stop_aware_tool_provider>();
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    {
      .id = "stop-aware-step",
      .title = "Stop aware",
      .assigned_tool = "stop_aware",
      .input = "{\"message\":\"hello\"}",
    },
  });
  auto executor = std::make_shared<tool_plan_executor>(provider);
  require(executor->capabilities(plan_step {}).cooperative_cancellation,
    "stop-aware tool provider advertises cooperative cancellation");
  std::stop_source stop_source;

  plan_runner runner({
    .planner = planner,
    .executor = executor,
    .stop_token = stop_source.get_token(),
  });

  const auto result = runner.run({ .goal = "call stop-aware tool" });
  require(result.completed, "stop-aware tool plan completes");
  require(provider->saw_stop_possible,
    "tool plan executor should forward plan_runner stop token to provider");
}

void failed_step_can_retry() {
  auto provider = std::make_shared<echo_tool_provider>();
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    {
      .id = "flaky",
      .title = "Flaky",
      .assigned_tool = "fail_once",
    },
  });
  auto executor = std::make_shared<tool_plan_executor>(provider);

  plan_runner runner({
    .planner = planner,
    .executor = executor,
    .policy = {
      .max_step_attempts = 2,
    },
  });

  const auto result = runner.run({ .goal = "retry flaky tool" });
  require(result.completed, "retried plan completes");
  require(result.value.steps.front().attempts == 2, "step attempted twice");
  require(result.final_output == "recovered", "retry output is returned");
}

void blocked_step_can_replan() {
  auto planner = std::make_shared<revising_planner>();
  auto provider = std::make_shared<echo_tool_provider>();
  auto executor = std::make_shared<tool_plan_executor>(provider);
  std::vector<plan_event_type> events;

  plan_runner runner({
    .planner = planner,
    .executor = executor,
    .policy = {
      .allow_replanning = true,
    },
    .observer = [&](const plan_event& event) {
      events.push_back(event.type);
    },
  });

  const auto result = runner.run({ .goal = "recover with a new plan" });
  require(result.completed, "replanned plan completes");
  require(planner->revised, "planner revise_plan was called");
  require(result.value.steps.front().id == "replacement", "revised step is used");
  require(result.final_output == "{\"ok\":true}", "revised step output is returned");
  require(!events.empty(), "observer received events");
}

void reflection_gate_can_retry_step_output() {
  namespace reflection = wuwe::agent::reflection;

  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    {
      .id = "answer",
      .title = "Answer",
    },
  });
  int calls = 0;
  auto executor = std::make_shared<function_plan_executor>(
    [&](const plan_step&, const plan_execution_context&) {
      ++calls;
      return plan_step_result::completed(calls == 1 ? "thin" : "good answer");
    });
  auto reflector = std::make_shared<reflection::rule_reflector>(reflection::rule_reflector_options {
    .required_substrings = { "good" },
  });
  auto reflection_runner = std::make_shared<reflection::reflection_runner>(reflection::reflection_runner_options {
    .reflector = reflector,
  });
  std::vector<plan_event_type> events;

  plan_runner runner({
    .planner = planner,
    .executor = executor,
    .policy = {
      .max_step_attempts = 2,
    },
    .reflection = {
      .runner = reflection_runner,
    },
    .observer = [&](const plan_event& event) {
      events.push_back(event.type);
    },
  });

  const auto result = runner.run({ .goal = "retry low-quality output" });
  require(result.completed, "reflection retry plan completes");
  require(calls == 2, "reflection retry reruns executor");
  require(result.final_output == "good answer", "retried output becomes final output");
  require(result.value.steps.front().metadata.at("reflection_action") == "pass",
    "final reflection pass is recorded");
  require(result.value.steps.front().metadata.at("reflection_issue_count") == "0",
    "final reflection issue count is updated");
  require(!result.value.steps.front().metadata.contains("reflection_issue"),
    "stale reflection issue metadata is cleared");
  require(std::count(events.begin(), events.end(), plan_event_type::step_reflected) == 2,
    "reflection event is emitted for each attempt");
}

void reflection_gate_can_trigger_replanning() {
  namespace reflection = wuwe::agent::reflection;

  auto planner = std::make_shared<reflection_replanning_planner>();
  auto executor = std::make_shared<function_plan_executor>(
    [](const plan_step& step, const plan_execution_context&) {
      if (step.id == "weak-answer") {
        return plan_step_result::completed("cannot satisfy goal");
      }
      return plan_step_result::completed("replacement answer");
    });
  auto reflector = std::make_shared<fixed_reflector>(reflection::reflection_result::fail(
    reflection::reflection_action::replan,
    {
      .severity = reflection::reflection_severity::error,
      .code = "wrong_path",
      .message = "The step result shows this plan path cannot satisfy the goal.",
    },
    0.2),
    true);
  auto reflection_runner = std::make_shared<reflection::reflection_runner>(reflection::reflection_runner_options {
    .reflector = reflector,
  });

  plan_runner runner({
    .planner = planner,
    .executor = executor,
    .policy = {
      .max_iterations = 4,
      .allow_replanning = true,
    },
    .reflection = {
      .runner = reflection_runner,
      .reflect_completed_steps = true,
    },
  });

  const auto result = runner.run({ .goal = "recover through reflection" });
  require(result.completed, "reflection replan plan completes after replacement passes");
  require(planner->revised, "reflection replan calls planner revise_plan");
  require(result.value.steps.front().id == "replacement", "replanned step replaces original");
  require(reflector->last_request.subject_type == "plan_step_result",
    "planning reflection builds a step-result request");
}

void invalid_plans_are_rejected_before_execution() {
  int executions = 0;
  auto executor = std::make_shared<function_plan_executor>(
    [&](const plan_step&, const plan_execution_context&) {
      ++executions;
      return plan_step_result::completed();
    });

  {
    auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
      { .id = "same", .title = "A" },
      { .id = "same", .title = "B" },
    });
    plan_runner runner({ .planner = planner, .executor = executor });
    require_throws([&] { runner.run({ .goal = "reject duplicates" }); }, "duplicate_step_id");
  }

  {
    auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
      { .id = "a", .title = "A", .depends_on = { "b" } },
      { .id = "b", .title = "B", .depends_on = { "a" } },
    });
    plan_runner runner({ .planner = planner, .executor = executor });
    require_throws([&] { runner.run({ .goal = "reject cycle" }); }, "dependency_cycle");
  }

  {
    auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
      { .id = "tool-step", .title = "Tool", .assigned_tool = "echo", .input = "not json" },
    });
    plan_runner runner({
      .planner = planner,
      .executor = executor,
      .validation = {
        .available_tools = { { .name = "echo", .description = "Echo" } },
      },
    });
    require_throws([&] { runner.run({ .goal = "reject invalid tool input" }); },
      "invalid_tool_input");
  }

  require(executions == 0, "invalid plans are rejected before executor runs");
}

void llm_planner_extracts_json_and_uses_tool_catalog() {
  fake_llm_client client(
    "```json\n"
    "{\"id\":\"llm-plan\",\"goal\":\"Use a tool\",\"steps\":[{\"id\":\"call\","
    "\"title\":\"Call echo\",\"description\":\"Call the echo tool\",\"depends_on\":[],"
    "\"assigned_tool\":\"echo\",\"input\":\"{\\\"message\\\":\\\"hello\\\"}\","
    "\"metadata\":{}}],\"metadata\":{}}\n"
    "```");

  llm_planner planner(client);
  auto output = planner.create_plan({
    .goal = "Use a tool",
    .max_steps = 4,
    .available_tools = {
      {
        .name = "echo",
        .description = "Echo arguments.",
        .parameters_json_schema = "{\"type\":\"object\"}",
      },
    },
  });

  require(output.id == "llm-plan", "llm planner parses fenced JSON");
  require(output.steps.size() == 1, "llm planner returns one step");
  require(output.steps.front().assigned_tool == "echo", "llm planner preserves known tool");
  require(client.last_request.messages.size() == 2, "llm planner sends system and user messages");
  require(client.last_request.messages.back().content.find("Available tools") != std::string::npos,
    "llm planner prompt includes tool catalog");
}

void llm_planner_rejects_unknown_tools() {
  fake_llm_client client(
    "{\"goal\":\"Bad tool\",\"steps\":[{\"id\":\"call\",\"title\":\"Call\","
    "\"assigned_tool\":\"made_up\",\"input\":\"{}\"}]}");

  llm_planner planner(client);
  require_throws([&] {
    planner.create_plan({
      .goal = "Bad tool",
      .available_tools = { { .name = "echo", .description = "Echo" } },
    });
  }, "unknown_tool");
}

void plan_serialization_round_trips_execution_state() {
  plan value {
    .id = "checkpoint",
    .goal = "Persist planning state",
    .steps = {
      {
        .id = "done",
        .title = "Done",
        .assigned_tool = "echo",
        .status = plan_step_status::completed,
        .input = "{}",
        .output = "ok",
        .attempts = 2,
        .metadata = { { "kind", "checkpoint" } },
      },
    },
    .metadata = { { "tenant", "t1" } },
  };

  const auto json = plan_to_json(value);
  const auto restored = plan_from_json(json);
  require(restored.id == value.id, "plan id round trips");
  require(restored.goal == value.goal, "plan goal round trips");
  require(restored.steps.front().status == plan_step_status::completed, "step status round trips");
  require(restored.steps.front().attempts == 2, "step attempts round trips");
  require(restored.steps.front().assigned_tool == "echo", "assigned tool round trips");
  require(restored.steps.front().metadata.at("kind") == "checkpoint", "step metadata round trips");
}

void runner_can_pause_and_resume_from_checkpoint() {
  std::vector<std::string> executed;
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    { .id = "one", .title = "One" },
    { .id = "two", .title = "Two", .depends_on = { "one" } },
  });
  auto executor = std::make_shared<function_plan_executor>(
    [&](const plan_step& step, const plan_execution_context&) {
      executed.push_back(step.id);
      return plan_step_result::completed("done-" + step.id);
    });

  plan_runner first_run({
    .planner = planner,
    .executor = executor,
    .policy = {
      .max_steps_per_run = 1,
    },
  });

  const auto paused = first_run.run({ .goal = "pause and resume" });
  require(!paused.completed, "first run pauses before completing");
  require(paused.stop_reason == plan_run_stop_reason::step_budget_exhausted,
    "first run stops because step budget is exhausted");
  require(paused.value.steps[0].status == plan_step_status::completed, "first step completed");
  require(paused.value.steps[1].status == plan_step_status::pending, "second step remains pending");

  plan_runner second_run({
    .planner = planner,
    .executor = executor,
  });
  const auto resumed = second_run.resume(paused.value);
  require(resumed.completed, "resumed run completes");
  require(resumed.stop_reason == plan_run_stop_reason::completed, "resumed run reports completion");
  require(resumed.final_output == "done-two", "resumed run returns final output");
  require(executed.size() == 2, "resume does not rerun completed step");
  require(executed[0] == "one" && executed[1] == "two", "resume executes remaining step only");
}

void runner_stops_for_approval_and_resumes_after_approval() {
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    {
      .id = "approve-me",
      .title = "Needs approval",
      .requires_approval = true,
    },
  });
  auto executor = std::make_shared<function_plan_executor>(
    [](const plan_step& step, const plan_execution_context&) {
      return plan_step_result::completed("approved-" + step.id);
    });

  bool approval_event = false;
  plan_runner runner({
    .planner = planner,
    .executor = executor,
    .observer = [&](const plan_event& event) {
      if (event.type == plan_event_type::step_approval_required) {
        approval_event = true;
      }
    },
  });

  auto waiting = runner.run({ .goal = "wait for approval" });
  require(!waiting.completed, "approval-gated run waits");
  require(waiting.stop_reason == plan_run_stop_reason::approval_required,
    "approval-gated run reports approval required");
  require(approval_event, "approval required event emitted");
  require(waiting.value.steps.front().attempts == 0, "approval-gated step is not executed early");

  waiting.value.steps.front().approved = true;
  const auto resumed = runner.resume(waiting.value);
  require(resumed.completed, "approved resumed plan completes");
  require(resumed.final_output == "approved-approve-me", "approved step output returned");
}

void runner_can_cancel_before_next_step() {
  int cancel_checks = 0;
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    { .id = "one", .title = "One" },
  });
  auto executor = std::make_shared<function_plan_executor>(
    [](const plan_step&, const plan_execution_context&) {
      return plan_step_result::completed("should-not-run");
    });

  plan_runner runner({
    .planner = planner,
    .executor = executor,
    .should_cancel = [&] {
      ++cancel_checks;
      return true;
    },
  });

  const auto result = runner.run({ .goal = "cancel" });
  require(!result.completed, "cancelled run does not complete");
  require(result.stop_reason == plan_run_stop_reason::cancelled, "cancelled run reports cancellation");
  require(result.steps_executed == 0, "cancelled run executes no steps");
  require(cancel_checks > 0, "cancellation callback checked");
}

void plan_store_saves_loads_and_erases_plans() {
  in_memory_plan_store memory_store;
  plan value {
    .id = "stored",
    .goal = "Persist",
    .steps = { { .id = "s1", .title = "Step" } },
  };

  memory_store.save(value);
  require(memory_store.load("stored").has_value(), "in-memory plan store loads saved plan");
  require(memory_store.list().size() == 1, "in-memory plan store lists saved plan");
  require(memory_store.erase("stored"), "in-memory plan store erases saved plan");
  require(!memory_store.load("stored").has_value(), "in-memory plan store no longer loads erased plan");

  const auto path = std::filesystem::temp_directory_path() / "wuwe-planning-tests-store.json";
  std::filesystem::remove(path);
  file_plan_store file_store(path);
  file_store.save(value);
  require(file_store.load("stored").has_value(), "file plan store loads saved plan");
  require(file_store.erase("stored"), "file plan store erases saved plan");
  std::filesystem::remove(path);
}

void runner_persists_and_traces_progress() {
  in_memory_plan_store store;
  std::vector<plan_trace_event> traces;
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    { .id = "trace", .title = "Trace" },
  });
  auto executor = std::make_shared<function_plan_executor>(
    [](const plan_step&, const plan_execution_context&) {
      return plan_step_result::completed("traced");
    });

  plan_runner runner({
    .planner = planner,
    .executor = executor,
    .store = &store,
    .trace_sink = [&](const plan_trace_event& event) {
      traces.push_back(event);
    },
  });

  const auto result = runner.run({ .goal = "trace me" });
  require(result.completed, "traced run completes");
  require(result.steps_completed == 1, "metrics count completed step");
  require(result.elapsed.count() >= 0, "elapsed metric is set");
  require(store.load(result.value.id).has_value(), "runner stores latest plan");
  require(!traces.empty(), "runner emits trace events");
}

void runner_executes_ready_steps_in_parallel() {
  std::atomic<int> active { 0 };
  std::atomic<int> maximum_active { 0 };
  std::atomic<bool> snapshot_is_consistent { true };
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    { .id = "a", .title = "A" },
    { .id = "b", .title = "B" },
  });
  auto executor = std::make_shared<function_plan_executor>(
    [&](const plan_step& step, const plan_execution_context& context) {
      for (const auto& current : context.current_plan.steps) {
        if (current.status != plan_step_status::running) {
          snapshot_is_consistent = false;
        }
      }
      const auto concurrent = active.fetch_add(1) + 1;
      auto observed = maximum_active.load();
      while (concurrent > observed &&
             !maximum_active.compare_exchange_weak(observed, concurrent)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      active.fetch_sub(1);
      return plan_step_result::completed(step.id);
    });

  plan_runner runner({
    .planner = planner,
    .executor = executor,
    .policy = {
      .max_parallel_steps = 2,
    },
  });

  const auto result = runner.run({ .goal = "parallel" });
  require(result.completed, "parallel ready steps complete");
  require(result.steps_executed == 2, "parallel run executes both ready steps");
  require(result.iterations == 2, "parallel run finishes after one execution iteration and one completion check");
  require(maximum_active.load() == 2, "ready steps overlap in execution");
  require(snapshot_is_consistent.load(),
    "parallel executors observe one immutable batch snapshot after all steps start");
}

void executor_capability_can_disable_concurrent_calls() {
  std::atomic<int> active { 0 };
  std::atomic<int> maximum_active { 0 };
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    { .id = "serial-a", .title = "Serial A" },
    { .id = "serial-b", .title = "Serial B" },
  });
  auto executor = std::make_shared<function_plan_executor>(
    [&](const plan_step& step, const plan_execution_context&) {
      const auto concurrent = active.fetch_add(1) + 1;
      auto observed = maximum_active.load();
      while (concurrent > observed &&
             !maximum_active.compare_exchange_weak(observed, concurrent)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      active.fetch_sub(1);
      return plan_step_result::completed(step.id);
    },
    plan_executor_capabilities { .concurrent_execution = false });

  plan_runner runner({
    .planner = planner,
    .executor = executor,
    .policy = { .max_parallel_steps = 2 },
  });

  const auto result = runner.run({ .goal = "serialize executor" });
  require(result.completed, "non-concurrent executor plan completes");
  require(maximum_active.load() == 1,
    "executor capability prevents concurrent calls to a non-thread-safe executor");
}

void executor_exception_becomes_a_step_failure() {
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    { .id = "throws", .title = "Throws" },
  });
  auto executor = std::make_shared<function_plan_executor>(
    [](const plan_step&, const plan_execution_context&) -> plan_step_result {
      throw std::runtime_error("executor failure");
    });

  plan_runner runner({ .planner = planner, .executor = executor });
  const auto result = runner.run({ .goal = "capture executor failure" });
  require(!result.completed, "executor exception fails the plan");
  require(result.stop_reason == plan_run_stop_reason::failed,
    "executor exception maps to a normal planning failure");
  require(result.value.steps.front().error.find("executor failure") != std::string::npos,
    "executor exception message is preserved");
  require(result.value.steps.front().metadata.at("executor_exception") == "true",
    "executor exception is identified in step metadata");
}

void runner_enforces_step_timeout_without_waiting_for_uncooperative_executor() {
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    { .id = "slow", .title = "Slow" },
  });
  auto executor = std::make_shared<function_plan_executor>(
    [](const plan_step&, const plan_execution_context&) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      return plan_step_result::completed("late");
    });

  plan_runner runner({
    .planner = planner,
    .executor = executor,
    .policy = {
      .max_step_attempts = 2,
      .step_timeout = std::chrono::milliseconds(20),
      .cancellation_poll_interval = std::chrono::milliseconds(2),
    },
  });

  const auto started = std::chrono::steady_clock::now();
  const auto result = runner.run({ .goal = "timeout" });
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - started);
  require(!result.completed, "timed out run does not complete");
  require(result.stop_reason == plan_run_stop_reason::failed, "timed out step fails run");
  require(result.value.steps.front().error.find("timeout") != std::string::npos,
    "timed out step records timeout error");
  require(elapsed < std::chrono::milliseconds(150),
    "step timeout returns before an uncooperative executor finishes");
  require(result.steps_timed_out == 1, "step timeout is counted");
  require(result.steps_detached == 1, "uncooperative timed out execution is reported as detached");
  require(result.value.steps.front().attempts == 1,
    "detached timed out execution is not retried while the prior side effect may still be running");
  require(result.value.steps.front().metadata.at("stop_reason") == "step_timeout",
    "timed out step exposes a stable stop reason");
  require(result.value.steps.front().metadata.at("execution_detached") == "true",
    "timed out step records detached execution");
}

void step_timeout_requests_cooperative_cancellation_and_exposes_deadline() {
  auto saw_stop = std::make_shared<std::atomic<bool>>(false);
  auto saw_deadline = std::make_shared<std::atomic<bool>>(false);
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    { .id = "cooperative", .title = "Cooperative" },
  });
  auto executor = std::make_shared<function_plan_executor>(
    [saw_stop, saw_deadline](const plan_step&, const plan_execution_context& context) {
      *saw_deadline = context.deadline.has_value() && context.remaining_time().count() >= 0;
      while (!context.cancellation_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      *saw_stop = true;
      return plan_step_result::failed("cancelled cooperatively");
    },
    plan_executor_capabilities { .cooperative_cancellation = true });

  plan_runner runner({
    .planner = planner,
    .executor = executor,
    .policy = {
      .step_timeout = std::chrono::milliseconds(20),
      .cancellation_poll_interval = std::chrono::milliseconds(2),
    },
  });

  const auto result = runner.run({ .goal = "cooperative timeout" });
  for (int attempt = 0; attempt < 100 && !saw_stop->load(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(result.steps_timed_out == 1, "cooperative step still observes the scheduler deadline");
  require(saw_deadline->load(), "executor receives the effective execution deadline");
  require(saw_stop->load(), "step timeout requests cooperative cancellation");
  require(result.value.steps.front().metadata.at("executor_cooperative_cancellation") == "true",
    "timeout metadata records executor cancellation capability");
}

void run_timeout_cancels_running_steps_with_distinct_stop_reason() {
  auto saw_stop = std::make_shared<std::atomic<bool>>(false);
  bool timeout_event = false;
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    { .id = "run-timeout", .title = "Run timeout" },
  });
  auto executor = std::make_shared<function_plan_executor>(
    [saw_stop](const plan_step&, const plan_execution_context& context) {
      while (!context.cancellation_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      *saw_stop = true;
      return plan_step_result::failed("run cancelled");
    },
    plan_executor_capabilities { .cooperative_cancellation = true });

  plan_runner runner({
    .planner = planner,
    .executor = executor,
    .policy = {
      .run_timeout = std::chrono::milliseconds(20),
      .cancellation_poll_interval = std::chrono::milliseconds(2),
    },
    .observer = [&](const plan_event& event) {
      timeout_event = timeout_event || event.type == plan_event_type::plan_timed_out;
    },
  });

  const auto started = std::chrono::steady_clock::now();
  const auto result = runner.run({ .goal = "run timeout" });
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - started);
  for (int attempt = 0; attempt < 100 && !saw_stop->load(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(!result.completed, "run timeout does not complete");
  require(result.stop_reason == plan_run_stop_reason::run_timeout,
    "run timeout has a distinct stable stop reason");
  require(timeout_event, "run timeout emits a dedicated event");
  require(elapsed < std::chrono::milliseconds(150),
    "run timeout returns without waiting for the running executor result");
  require(saw_stop->load(), "run timeout propagates cancellation to the running step");
  require(result.value.steps.front().metadata.at("stop_reason") == "run_timeout",
    "running step records run timeout interruption");
}

void external_stop_token_cancels_a_running_plan() {
  auto saw_stop = std::make_shared<std::atomic<bool>>(false);
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    { .id = "external-cancel", .title = "External cancel" },
  });
  auto executor = std::make_shared<function_plan_executor>(
    [saw_stop](const plan_step&, const plan_execution_context& context) {
      while (!context.cancellation_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      *saw_stop = true;
      return plan_step_result::failed("cancelled");
    },
    plan_executor_capabilities { .cooperative_cancellation = true });
  std::stop_source stop_source;
  std::jthread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop_source.request_stop();
  });

  plan_runner runner({
    .planner = planner,
    .executor = executor,
    .policy = {
      .cancellation_poll_interval = std::chrono::milliseconds(2),
    },
    .stop_token = stop_source.get_token(),
  });

  const auto result = runner.run({ .goal = "external cancellation" });
  for (int attempt = 0; attempt < 100 && !saw_stop->load(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(!result.completed, "externally cancelled plan does not complete");
  require(result.stop_reason == plan_run_stop_reason::cancelled,
    "external stop token maps to plan cancellation");
  require(saw_stop->load(), "external cancellation reaches the active executor");
  require(result.value.steps.front().metadata.at("stop_reason") == "cancelled",
    "active step records external cancellation");
}

void resume_does_not_duplicate_detached_execution_without_explicit_policy() {
  int executions = 0;
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    { .id = "detached", .title = "Detached" },
  });
  auto executor = std::make_shared<function_plan_executor>(
    [&](const plan_step&, const plan_execution_context&) {
      ++executions;
      return plan_step_result::completed("reconciled");
    });
  plan interrupted {
    .id = "detached-plan",
    .goal = "resume safely",
    .steps = {
      {
        .id = "detached",
        .title = "Detached",
        .status = plan_step_status::running,
        .metadata = { { "execution_detached", "true" } },
      },
    },
  };

  plan_runner safe_runner({ .planner = planner, .executor = executor });
  const auto waiting = safe_runner.resume(interrupted);
  require(!waiting.completed, "detached execution requires host reconciliation before resume");
  require(waiting.stop_reason == plan_run_stop_reason::no_ready_step,
    "detached running step remains non-runnable by default");
  require(executions == 0, "default resume does not duplicate a detached side effect");

  plan_runner explicit_runner({
    .planner = planner,
    .executor = executor,
    .policy = { .reset_detached_steps_on_resume = true },
  });
  const auto resumed = explicit_runner.resume(waiting.value);
  require(resumed.completed, "host can explicitly permit rerunning a reconciled detached step");
  require(executions == 1, "explicit detached reset runs the step once");
  require(!resumed.value.steps.front().metadata.contains("execution_detached"),
    "new execution clears stale interruption metadata");
}

void approval_provider_can_auto_approve() {
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    { .id = "approved", .title = "Approved", .requires_approval = true },
  });
  auto executor = std::make_shared<function_plan_executor>(
    [](const plan_step&, const plan_execution_context&) {
      return plan_step_result::completed("ok");
    });

  plan_runner runner({
    .planner = planner,
    .executor = executor,
    .approval_provider = [](const plan_step&, const plan&) {
      return plan_approval_result {
        .approved = true,
        .reason = "test approval",
        .metadata = { { "approved_by", "test" } },
      };
    },
  });

  const auto result = runner.run({ .goal = "auto approve" });
  require(result.completed, "approval provider allows execution");
  require(result.value.steps.front().approved, "approval provider marks step approved");
  require(result.value.steps.front().metadata.at("approved_by") == "test", "approval metadata is recorded");
}

void policy_hook_can_require_approval_or_deny() {
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    { .id = "needs-policy-approval", .title = "Policy approval" },
    { .id = "denied", .title = "Denied", .depends_on = { "needs-policy-approval" } },
  });
  auto executor = std::make_shared<function_plan_executor>(
    [](const plan_step& step, const plan_execution_context&) {
      return plan_step_result::completed(step.id);
    });

  plan_runner runner({
    .planner = planner,
    .executor = executor,
    .policy = { .continue_after_step_failure = true },
    .policy_check = [](const plan_step& step, const plan&) {
      if (step.id == "needs-policy-approval" && !step.approved) {
        return plan_policy_check { .decision = plan_policy_decision::require_approval, .reason = "policy" };
      }
      if (step.id == "denied") {
        return plan_policy_check { .decision = plan_policy_decision::deny, .reason = "denied by policy" };
      }
      return plan_policy_check {};
    },
  });

  auto waiting = runner.run({ .goal = "policy" });
  require(waiting.stop_reason == plan_run_stop_reason::approval_required,
    "policy can require approval");

  waiting.value.steps.front().approved = true;
  const auto resumed = runner.resume(waiting.value);
  require(!resumed.completed, "policy denied run does not complete");
  require(resumed.value.steps.back().status == plan_step_status::blocked, "policy can block step");
}

void repair_normalizes_common_plan_issues() {
  plan value {
    .goal = "Repair",
    .steps = {
      { .title = "First" },
      { .id = "dup", .title = "A", .depends_on = { "missing" } },
      { .id = "dup", .title = "B" },
    },
  };

  normalize_plan(value);
  require(value.steps[0].id == "step-1", "repair fills missing id");
  require(value.steps[1].id == "dup", "repair keeps first duplicate id");
  require(value.steps[2].id == "dup-2", "repair makes duplicate id unique");
  require(value.steps[1].depends_on.empty(), "repair removes unknown dependency");
  require(validate_plan(value).valid(), "repaired plan validates");
}

void typed_io_and_artifacts_flow_between_steps() {
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    { .id = "produce", .title = "Produce" },
    { .id = "consume", .title = "Consume", .depends_on = { "produce" }, .input_from_steps = { "produce" } },
  });
  auto executor = std::make_shared<function_plan_executor>(
    [](const plan_step& step, const plan_execution_context& context) {
      if (step.id == "produce") {
        return plan_step_result {
          .status = plan_step_status::completed,
          .output = "{\"value\":42}",
          .output_json = { { "value", 42 } },
          .artifacts = { { "answer", 42 } },
        };
      }
      require(context.artifacts.at("answer") == 42, "artifact is available to downstream step");
      require(step.input_json["steps"]["produce"]["value"] == 42, "step input includes upstream output");
      return plan_step_result::completed("consumed");
    });

  plan_runner runner({ .planner = planner, .executor = executor });
  const auto result = runner.run({ .goal = "artifacts" });
  require(result.completed, "artifact plan completes");
  require(result.value.artifacts.at("answer") == 42, "plan stores produced artifact");
}

void agent_executor_hands_off_to_registered_agent() {
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    { .id = "delegate", .title = "Delegate", .assigned_agent = "researcher" },
  });
  auto agent_executor = std::make_shared<agent_plan_executor>();
  agent_executor->add_agent("researcher",
    [](const plan_step& step, const plan_execution_context&) {
      return plan_step_result::completed("agent:" + *step.assigned_agent);
    });

  plan_runner runner({ .planner = planner, .executor = agent_executor });
  const auto result = runner.run({
    .goal = "handoff",
    .available_agents = { "researcher" },
  });
  require(result.completed, "agent handoff plan completes");
  require(result.final_output == "agent:researcher", "agent executor returns agent output");
}

void prioritization_orders_ready_steps_and_round_trips_signals() {
  std::vector<std::string> executed;
  const auto deadline = std::chrono::system_clock::now() + std::chrono::minutes(5);
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    {
      .id = "cheap",
      .title = "Cheap",
      .priority = 1.0,
      .urgency = 0.1,
      .expected_value = 0.2,
      .estimated_cost = 0.1,
    },
    {
      .id = "important",
      .title = "Important",
      .priority = 5.0,
      .urgency = 1.0,
      .expected_value = 2.0,
      .estimated_cost = 0.5,
      .deadline = deadline,
    },
  });
  auto executor = std::make_shared<function_plan_executor>(
    [&](const plan_step& step, const plan_execution_context&) {
      executed.push_back(step.id);
      return plan_step_result::completed(step.id);
    });
  plan_runner runner({
    .planner = planner,
    .executor = executor,
    .policy = {
      .max_steps_per_run = 1,
      .max_parallel_steps = 1,
    },
  });
  const auto result = runner.run({ .goal = "prioritize" });
  require(executed == std::vector<std::string> { "important" } &&
      result.stop_reason == plan_run_stop_reason::step_budget_exhausted,
    "ready steps are dynamically ordered by formal priority signals");

  const auto json = plan_step_to_json(planner->create_plan({ .goal = "serialize" }).steps[1]);
  const auto restored = plan_step_from_json(json);
  require(restored.priority == 5.0 && restored.urgency == 1.0 &&
      restored.expected_value == 2.0 && restored.estimated_cost == 0.5 &&
      restored.deadline.has_value(),
    "priority signals and deadlines round trip through the public plan codec");

  plan value {
    .goal = "custom",
    .steps = {
      { .id = "first", .title = "First" },
      { .id = "second", .title = "Second" },
    },
  };
  plan_prioritizer custom({}, [](const plan_step& step, const plan&,
      std::chrono::system_clock::time_point) {
    return step.id == "second" ? 10.0 : 0.0;
  });
  require(custom.order(value, { 0, 1 }) == std::vector<std::size_t> { 1, 0 },
    "applications can replace the default priority scoring policy");
}

void function_plan_executors_reject_empty_callbacks() {
  bool rejected = false;
  try {
    (void)function_plan_executor({});
  }
  catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "function plan executors fail fast on empty callbacks");

  bool empty_tool_callbacks = false;
  try {
    (void)tool_plan_executor(
      tool_plan_executor::tools_callback {},
      tool_plan_executor::invoke_callback {});
  }
  catch (const std::invalid_argument&) {
    empty_tool_callbacks = true;
  }
  bool empty_agent_callback = false;
  try {
    agent_plan_executor executor;
    executor.add_agent("agent", {});
  }
  catch (const std::invalid_argument&) {
    empty_agent_callback = true;
  }
  require(empty_tool_callbacks && empty_agent_callback,
    "tool and agent plan executors reject incomplete callback contracts");
}

void runner_rejects_invalid_execution_policies() {
  auto planner = std::make_shared<static_planner>(std::vector<plan_step> {
    { .id = "step", .title = "Step" },
  });
  auto executor = std::make_shared<function_plan_executor>(
    [](const plan_step&, const plan_execution_context&) {
      return plan_step_result::completed("done");
    });
  bool negative_timeout = false;
  try {
    (void)plan_runner({
      .planner = planner,
      .executor = executor,
      .policy = { .step_timeout = std::chrono::milliseconds(-1) },
    });
  }
  catch (const std::invalid_argument&) {
    negative_timeout = true;
  }
  bool negative_weight = false;
  try {
    (void)plan_prioritizer({ .priority_weight = -1.0 });
  }
  catch (const std::invalid_argument&) {
    negative_weight = true;
  }
  require(negative_timeout && negative_weight,
    "planning rejects invalid timeout and prioritization policies at construction");
}

} // namespace

int main() {
  static_plan_respects_dependencies();
  tool_executor_invokes_provider();
  tool_executor_forwards_stop_token_to_provider();
  failed_step_can_retry();
  blocked_step_can_replan();
  reflection_gate_can_retry_step_output();
  reflection_gate_can_trigger_replanning();
  invalid_plans_are_rejected_before_execution();
  llm_planner_extracts_json_and_uses_tool_catalog();
  llm_planner_rejects_unknown_tools();
  plan_serialization_round_trips_execution_state();
  runner_can_pause_and_resume_from_checkpoint();
  runner_stops_for_approval_and_resumes_after_approval();
  runner_can_cancel_before_next_step();
  plan_store_saves_loads_and_erases_plans();
  runner_persists_and_traces_progress();
  runner_executes_ready_steps_in_parallel();
  executor_capability_can_disable_concurrent_calls();
  executor_exception_becomes_a_step_failure();
  runner_enforces_step_timeout_without_waiting_for_uncooperative_executor();
  step_timeout_requests_cooperative_cancellation_and_exposes_deadline();
  run_timeout_cancels_running_steps_with_distinct_stop_reason();
  external_stop_token_cancels_a_running_plan();
  resume_does_not_duplicate_detached_execution_without_explicit_policy();
  approval_provider_can_auto_approve();
  policy_hook_can_require_approval_or_deny();
  repair_normalizes_common_plan_issues();
  typed_io_and_artifacts_flow_between_steps();
  agent_executor_hands_off_to_registered_agent();
  prioritization_orders_ready_steps_and_round_trips_signals();
  function_plan_executors_reject_empty_callbacks();
  runner_rejects_invalid_execution_policies();
}
