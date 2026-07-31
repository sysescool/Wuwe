#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <wuwe/agent/core/observability.hpp>
#include <wuwe/agent/multi_agent/multi_agent.hpp>
#include <wuwe/agent/multi_agent/planning_adapter.hpp>
#include <wuwe/common/print.h>

namespace {
namespace ma = wuwe::agent::multi_agent;

void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::shared_ptr<ma::function_agent_executor> echo(std::string prefix) {
  return std::make_shared<ma::function_agent_executor>(
    [prefix = std::move(prefix)](
      const ma::agent_task_request& request, const ma::agent_execution_context&) {
      return ma::agent_task_result {
        .output = prefix + request.input,
        .artifacts = { {
          .id = request.id + ":artifact",
          .name = "answer",
          .content = prefix + request.input,
        } },
      };
    });
}

ma::agent_descriptor descriptor(
  std::string id, std::string skill, std::size_t max_concurrency = 1) {
  return {
    .id = id,
    .name = id,
    .role = "specialist",
    .skills = { { .id = skill, .name = skill } },
    .max_concurrency = max_concurrency,
  };
}

void registry_routes_by_skill_and_lifecycle() {
  auto registry = std::make_shared<ma::agent_registry>();
  registry->add(descriptor("writer", "write"), echo("written:"));
  registry->add(descriptor("reviewer", "review"), echo("reviewed:"));
  ma::team_runtime runtime({ .registry = registry });

  const auto routed = runtime.run({
    .input = "draft",
    .required_skills = { "review" },
  });
  require(routed && routed.agent_id == "reviewer" && routed.output == "reviewed:draft",
    "team runtime routes tasks by declared skills");
  registry->set_availability("reviewer", ma::agent_availability::draining);
  const auto draining = runtime.run({
    .input = "draft",
    .preferred_agent = "reviewer",
  });
  require(!draining && draining.error_code == ma::agent_task_error_code::agent_unavailable,
    "draining agents reject new work with a stable error");
}

void registry_enforces_capacity_and_releases_leases() {
  auto registry = std::make_shared<ma::agent_registry>();
  std::atomic<bool> entered { false };
  std::atomic<bool> release { false };
  registry->add(descriptor("serial", "work"),
    std::make_shared<ma::function_agent_executor>(
      [&](const ma::agent_task_request&, const ma::agent_execution_context&) {
        entered = true;
        while (!release)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return ma::agent_task_result { .output = "done" };
      }));
  ma::team_runtime runtime({ .registry = registry });
  auto first = runtime.run_async({ .input = "first", .preferred_agent = "serial" });
  while (!entered)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const auto second = runtime.run({ .input = "second", .preferred_agent = "serial" });
  require(!second && second.error_code == ma::agent_task_error_code::capacity_exhausted,
    "agent max_concurrency is enforced");
  release = true;
  require(static_cast<bool>(first.get()), "active task completes");
  require(static_cast<bool>(runtime.run({ .input = "third", .preferred_agent = "serial" })),
    "completed tasks release their agent lease");
}

void registry_rejects_inconsistent_concurrency_contracts() {
  bool rejected = false;
  try {
    ma::agent_registry registry;
    registry.add(descriptor("serial", "work", 2),
      std::make_shared<ma::function_agent_executor>(
        [](const ma::agent_task_request&, const ma::agent_execution_context&) {
          return ma::agent_task_result {};
        },
        ma::agent_executor_capabilities { .concurrent_execution = false }));
  }
  catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "non-concurrent executors cannot advertise multiple concurrent tasks");
}

void session_task_admission_is_atomic() {
  auto registry = std::make_shared<ma::agent_registry>();
  std::atomic<bool> entered { false };
  std::atomic<bool> release { false };
  registry->add(descriptor("worker", "work"),
    std::make_shared<ma::function_agent_executor>(
      [&](const ma::agent_task_request&, const ma::agent_execution_context&) {
        entered = true;
        while (!release)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return ma::agent_task_result { .output = "done" };
      }));
  ma::team_runtime runtime({ .registry = registry });
  auto first = runtime.run_async({
    .id = "shared-task",
    .session_id = "admission-session",
    .input = "first",
    .preferred_agent = "worker",
  });
  while (!entered)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const auto duplicate = runtime.run({
    .id = "shared-task",
    .session_id = "admission-session",
    .input = "second",
    .preferred_agent = "worker",
  });
  require(!duplicate && duplicate.error_code == ma::agent_task_error_code::invalid_request &&
            runtime.find_session("admission-session")->snapshot().tasks.at("shared-task") ==
              ma::agent_task_status::working,
    "concurrent duplicate task IDs are rejected without overwriting active state");
  release = true;
  require(static_cast<bool>(first.get()), "the admitted task completes normally");
  const auto completed_duplicate = runtime.run({
    .id = "shared-task",
    .session_id = "admission-session",
    .input = "third",
    .preferred_agent = "worker",
  });
  require(!completed_duplicate &&
            runtime.find_session("admission-session")->snapshot().tasks.at("shared-task") ==
              ma::agent_task_status::completed,
    "completed task IDs cannot be executed a second time");

  std::atomic<int> attempts { 0 };
  registry->add(descriptor("retry", "retry"),
    std::make_shared<ma::function_agent_executor>(
      [&](const ma::agent_task_request&, const ma::agent_execution_context&) {
        if (++attempts == 1) {
          return ma::agent_task_result {
            .status = ma::agent_task_status::failed,
            .error = "transient",
          };
        }
        return ma::agent_task_result { .output = "recovered" };
      }));
  const ma::agent_task_request retry_request {
    .id = "retry-task",
    .session_id = "admission-session",
    .input = "retry",
    .preferred_agent = "retry",
  };
  require(!runtime.run(retry_request) && runtime.run(retry_request),
    "failed task IDs remain reusable for explicit retry");
}

void sessions_and_consensus_are_first_class() {
  auto registry = std::make_shared<ma::agent_registry>();
  registry->add(descriptor("a", "vote"), echo("same:"));
  registry->add(descriptor("b", "vote"), echo("same:"));
  registry->add(descriptor("c", "vote"), echo("different:"));
  ma::team_runtime runtime({ .registry = registry, .max_parallel_tasks = 3 });
  auto session = runtime.create_session("shared");
  session->set("topic", "agents");

  const auto consensus = runtime.reach_consensus({
    .task = { .session_id = "shared", .input = "answer", .required_skills = { "vote" } },
    .participant_agents = { "a", "b", "c" },
    .minimum_successful_agents = 2,
    .minimum_agreement = 2,
  });
  const auto snapshot = session->snapshot();
  require(consensus && consensus.final_result.output == "same:answer" &&
            consensus.final_result.metadata.at("matching_votes") == "2",
    "default consensus performs deterministic exact-output voting");
  require(snapshot.shared_state.at("topic") == "agents" && snapshot.messages.size() == 7 &&
            snapshot.artifacts.size() == 3 &&
            snapshot.tasks.at(consensus.final_result.task_id) == ma::agent_task_status::completed,
    "shared sessions commit participant work and the final consensus task");
}

void synchronous_executor_contract_is_enforced() {
  auto registry = std::make_shared<ma::agent_registry>();
  registry->add(descriptor("pending", "work"),
    std::make_shared<ma::function_agent_executor>(
      [](const ma::agent_task_request&, const ma::agent_execution_context&) {
        return ma::agent_task_result { .status = ma::agent_task_status::working };
      }));
  registry->add(descriptor("invalid-artifact", "artifact"),
    std::make_shared<ma::function_agent_executor>(
      [](const ma::agent_task_request&, const ma::agent_execution_context&) {
        return ma::agent_task_result {
          .output = "must not be committed",
          .artifacts = { { .name = "missing id" } },
        };
      }));
  ma::team_runtime runtime({ .registry = registry });

  const auto pending = runtime.run({ .input = "work", .preferred_agent = "pending" });
  require(!pending && pending.status == ma::agent_task_status::failed &&
            pending.error_code == ma::agent_task_error_code::execution_failed,
    "synchronous executors cannot release capacity with an in-progress result");
  const auto invalid = runtime.run({
    .session_id = "invalid-result-session",
    .input = "work",
    .preferred_agent = "invalid-artifact",
  });
  const auto snapshot = runtime.find_session("invalid-result-session")->snapshot();
  require(!invalid && invalid.error_code == ma::agent_task_error_code::execution_failed &&
            invalid.output.empty() && snapshot.artifacts.empty() && snapshot.messages.size() == 1,
    "invalid executor artifacts fail atomically without partial result publication");
}

void consensus_resolver_failures_are_structured() {
  auto registry = std::make_shared<ma::agent_registry>();
  registry->add(descriptor("a", "vote"), echo("answer:"));
  ma::team_runtime runtime({ .registry = registry });
  const auto result = runtime.reach_consensus({
    .task = {
      .id = "resolver-consensus",
      .session_id = "resolver-session",
      .input = "value",
    },
    .participant_agents = { "a" },
    .resolver = [](const auto&, const auto&) -> ma::agent_task_result {
      throw std::runtime_error("resolver unavailable");
    },
  });
  const auto snapshot = runtime.find_session("resolver-session")->snapshot();
  require(!result && result.error_code == ma::agent_task_error_code::execution_failed &&
            result.error.find("resolver unavailable") != std::string::npos &&
            snapshot.tasks.at("resolver-consensus") == ma::agent_task_status::failed,
    "resolver exceptions become structured consensus failures");
  const auto retried = runtime.reach_consensus({
    .task = {
      .id = "resolver-consensus",
      .session_id = "resolver-session",
      .input = "value",
    },
    .participant_agents = { "a" },
    .resolver = [](const auto& results, const auto&) {
      return results.front();
    },
  });
  require(retried && retried.final_result.task_id == "resolver-consensus",
    "failed consensus tasks can retry with isolated participant round IDs");
}

void consensus_cancellation_is_preserved() {
  auto registry = std::make_shared<ma::agent_registry>();
  registry->add(descriptor("a", "vote"), echo("answer:"));
  std::vector<ma::team_event_type> events;
  ma::team_runtime runtime({
    .registry = registry,
    .observer = [&](const ma::team_event& event) { events.push_back(event.type); },
  });
  std::stop_source stop_source;
  stop_source.request_stop();
  const auto result = runtime.reach_consensus({
    .task = {
      .id = "cancelled-consensus",
      .session_id = "cancelled-consensus-session",
      .input = "value",
    },
    .participant_agents = { "a" },
  }, stop_source.get_token());
  const auto snapshot = runtime.find_session("cancelled-consensus-session")->snapshot();
  require(!result && result.error_code == ma::agent_task_error_code::cancelled &&
            snapshot.tasks.at("cancelled-consensus") == ma::agent_task_status::cancelled &&
            registry->find("a")->active_tasks == 0 &&
            events == std::vector { ma::team_event_type::consensus_cancelled },
    "pre-dispatch consensus cancellation remains distinct from failed agreement");
}

void telemetry_and_planning_adapter_are_integrated() {
  auto registry = std::make_shared<ma::agent_registry>();
  registry->add(descriptor("writer", "write"), echo("plan:"));
  auto runtime = std::make_shared<ma::team_runtime>(ma::team_runtime_options {
    .registry = registry,
    .observer = [](const ma::team_event&) { throw std::runtime_error("telemetry down"); },
  });
  const auto direct = runtime->run({ .input = "value", .preferred_agent = "writer" });
  require(direct && direct.metadata.at("telemetry_error_count") == "4",
    "team telemetry failures are isolated and reported");

  ma::team_plan_executor executor(runtime);
  wuwe::agent::planning::plan plan { .id = "plan-1", .goal = "write" };
  wuwe::agent::planning::plan_step step {
    .id = "step-1",
    .title = "write",
    .assigned_agent = "writer",
    .input = "draft",
  };
  const std::map<std::string, nlohmann::json> artifacts;
  const auto result = executor.execute(step,
    {
      .current_plan = plan,
      .artifacts = artifacts,
    });
  require(result.status == wuwe::agent::planning::plan_step_status::completed &&
            result.output == "plan:draft" && result.metadata.at("agent_id") == "writer",
    "Planning dispatches assigned steps through team runtime");
}

void common_telemetry_propagates_trace_without_content() {
  auto registry = std::make_shared<ma::agent_registry>();
  registry->add(descriptor("writer", "write"), echo("secret-output:"));
  wuwe::agent::observability::in_memory_event_sink sink;
  std::vector<ma::team_event> typed_events;
  ma::team_runtime runtime({
    .registry = registry,
    .observer = [&](const ma::team_event& event) { typed_events.push_back(event); },
    .event_sink = &sink,
  });
  const auto result = runtime.run({
    .input = "secret-input",
    .preferred_agent = "writer",
    .metadata = { { "trace_id", "trace-1" } },
  });
  const auto events = sink.events();
  require(
    result && events.size() == 4, "team runtime publishes lifecycle events to the common sink");
  for (const auto& event : events) {
    require(event.trace_id == "trace-1" &&
              std::none_of(event.attributes.begin(),
                event.attributes.end(),
                [](const auto& item) {
                  return item.second.find("secret-input") != std::string::npos ||
                         item.second.find("secret-output") != std::string::npos;
                }),
      "common team telemetry propagates trace IDs without task content");
  }
  for (const auto& event : typed_events) {
    require(event.message.find("secret-input") == std::string::npos &&
              event.message.find("secret-output") == std::string::npos,
      "typed lifecycle events do not carry successful task content");
  }
}

void runtime_enforces_global_concurrency_and_bounded_execution() {
  auto registry = std::make_shared<ma::agent_registry>();
  std::atomic<int> active { 0 };
  std::atomic<int> maximum_active { 0 };
  std::atomic<bool> saw_deadline { false };
  registry->add(descriptor("parallel", "work", 8),
    std::make_shared<ma::function_agent_executor>(
      [&](const ma::agent_task_request&, const ma::agent_execution_context& context) {
        saw_deadline = saw_deadline || context.deadline.has_value();
        const auto current = active.fetch_add(1) + 1;
        auto maximum = maximum_active.load();
        while (current > maximum && !maximum_active.compare_exchange_weak(maximum, current)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
        active.fetch_sub(1);
        return ma::agent_task_result { .output = "done" };
      }));
  ma::team_runtime runtime({
    .registry = registry,
    .max_parallel_tasks = 2,
    .default_task_timeout = std::chrono::milliseconds(500),
  });
  std::vector<std::future<ma::agent_task_result>> futures;
  for (int index = 0; index < 6; ++index) {
    futures.push_back(runtime.run_async({
      .input = std::to_string(index),
      .preferred_agent = "parallel",
    }));
  }
  for (auto& future : futures) {
    require(static_cast<bool>(future.get()), "globally limited task completes");
  }
  require(maximum_active == 2 && saw_deadline,
    "max_parallel_tasks is a runtime-wide limit and contexts expose deadlines");

  std::atomic<bool> finished { false };
  auto timeout_registry = std::make_shared<ma::agent_registry>();
  timeout_registry->add(descriptor("slow", "slow"),
    std::make_shared<ma::function_agent_executor>(
      [&](const ma::agent_task_request&, const ma::agent_execution_context& context) {
        require(context.deadline.has_value() && context.remaining_time().count() >= 0,
          "timed execution receives a bounded operation context");
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        finished = true;
        return ma::agent_task_result { .output = "late" };
      }));
  ma::team_runtime timeout_runtime({ .registry = timeout_registry });
  const auto started = std::chrono::steady_clock::now();
  const auto timed = timeout_runtime.run({
    .id = "timed-task",
    .session_id = "timed-session",
    .input = "slow",
    .preferred_agent = "slow",
    .timeout = std::chrono::milliseconds(20),
  });
  require(!timed && timed.status == ma::agent_task_status::timed_out &&
            timed.error_code == ma::agent_task_error_code::timed_out && timed.detached &&
            std::chrono::steady_clock::now() - started < std::chrono::milliseconds(100),
    "uncooperative executors time out promptly with explicit detached state");
  const auto duplicate = timeout_runtime.run({
    .id = "timed-task",
    .session_id = "timed-session",
    .input = "duplicate",
    .preferred_agent = "slow",
  });
  require(!duplicate && duplicate.error_code == ma::agent_task_error_code::invalid_request,
    "timed-out task IDs cannot duplicate detached work");
  while (!finished)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  for (int attempt = 0; attempt < 500 && timeout_registry->find("slow")->active_tasks != 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(timeout_registry->find("slow")->active_tasks == 0,
    "detached completion releases runtime and agent capacity safely");
}

void function_agent_executor_rejects_empty_callbacks() {
  bool rejected = false;
  try {
    (void)ma::function_agent_executor({});
  }
  catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "function agent executors fail fast on empty callbacks");
}

void run_parallel_uses_a_bounded_coordinator_worker_set() {
  auto registry = std::make_shared<ma::agent_registry>();
  registry->add(descriptor("parallel", "work", 16), echo("done:"));
  std::mutex mutex;
  std::set<std::thread::id> coordinator_threads;
  ma::team_runtime runtime({
    .registry = registry,
    .observer =
      [&](const ma::team_event& event) {
        if (event.type == ma::team_event_type::task_submitted) {
          std::scoped_lock lock(mutex);
          coordinator_threads.insert(std::this_thread::get_id());
        }
      },
    .max_parallel_tasks = 3,
  });
  std::vector<ma::agent_task_request> requests;
  for (int index = 0; index < 40; ++index) {
    requests.push_back({
      .input = std::to_string(index),
      .preferred_agent = "parallel",
    });
  }
  const auto results = runtime.run_parallel(std::move(requests));
  require(results.size() == 40 &&
            std::all_of(results.begin(),
              results.end(),
              [](const auto& result) { return static_cast<bool>(result); }) &&
            coordinator_threads.size() <= 3,
    "run_parallel bounds coordinator threads by max_parallel_tasks");
}

void strict_telemetry_failure_releases_task_admission() {
  auto registry = std::make_shared<ma::agent_registry>();
  registry->add(descriptor("writer", "write"), echo("done:"));
  std::atomic<int> failures_remaining { 1 };
  ma::team_runtime runtime({
    .registry = registry,
    .observer =
      [&](const ma::team_event&) {
        if (failures_remaining.fetch_sub(1) > 0) {
          throw std::runtime_error("strict telemetry unavailable");
        }
      },
    .telemetry_failure_mode = wuwe::agent::observability::telemetry_failure_mode::propagate,
  });
  bool propagated = false;
  try {
    (void)runtime.run({
      .id = "retryable",
      .session_id = "strict-session",
      .input = "first",
      .preferred_agent = "writer",
    });
  }
  catch (const std::runtime_error&) {
    propagated = true;
  }
  const auto retried = runtime.run({
    .id = "retryable",
    .session_id = "strict-session",
    .input = "second",
    .preferred_agent = "writer",
  });
  require(propagated && retried,
    "strict telemetry exceptions do not strand task admission in an active state");
}

void run(const char* name, void (*test)()) {
  test();
  wuwe::println("[PASS] {}", name);
}
} // namespace

int main() {
  try {
    run("registry routes by skill and lifecycle", registry_routes_by_skill_and_lifecycle);
    run("registry enforces capacity", registry_enforces_capacity_and_releases_leases);
    run("registry rejects inconsistent concurrency contracts",
      registry_rejects_inconsistent_concurrency_contracts);
    run("session task admission is atomic", session_task_admission_is_atomic);
    run("sessions and consensus are first class", sessions_and_consensus_are_first_class);
    run("synchronous executor contract is enforced", synchronous_executor_contract_is_enforced);
    run("consensus resolver failures are structured", consensus_resolver_failures_are_structured);
    run("consensus cancellation is preserved", consensus_cancellation_is_preserved);
    run("telemetry and Planning adapter are integrated",
      telemetry_and_planning_adapter_are_integrated);
    run("common telemetry propagates trace without content",
      common_telemetry_propagates_trace_without_content);
    run("runtime concurrency and bounded execution",
      runtime_enforces_global_concurrency_and_bounded_execution);
    run("function executor validation", function_agent_executor_rejects_empty_callbacks);
    run("bounded run_parallel coordinator workers",
      run_parallel_uses_a_bounded_coordinator_worker_set);
    run(
      "strict telemetry releases task admission", strict_telemetry_failure_releases_task_admission);
  }
  catch (const std::exception& ex) {
    wuwe::println("[FAIL] {}", ex.what());
    return 1;
  }
}
