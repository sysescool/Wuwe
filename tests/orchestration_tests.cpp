#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <wuwe/agent/orchestration/flow_primitives.hpp>
#include <wuwe/common/print.h>

namespace {
using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

class test_llm_client final : public wuwe::llm_client {
public:
  wuwe::llm_response complete(const wuwe::llm_request& request) override {
    return { .content =
               request.messages.empty() ? std::string {} : request.messages.back().content };
  }

  wuwe::llm_response complete(
    const wuwe::llm_request& request, std::stop_token stop_token) override {
    saw_stop_token = stop_token.stop_possible();
    return wuwe::llm_client::complete(request, stop_token);
  }

  bool saw_stop_token { false };
};

void flow_context_propagates_to_steps_and_llm_calls() {
  auto client = std::make_shared<test_llm_client>();
  bool step_saw_cancellation = false;
  auto pipeline = client | [&](const std::string& input, const wuwe::flow_context& context) {
    step_saw_cancellation = context.cancellation_requested();
    return input;
  };
  std::stop_source stop_source;
  stop_source.request_stop();
  const auto response = pipeline.invoke("cancel me", stop_source.get_token());
  require(step_saw_cancellation && client->saw_stop_token && !response,
    "flow context and cancellation reach ordinary steps and implicit LLM calls");

  auto parallel = client | [](int value) { return value; } |
                  wuwe::fan_out([](const int& value) { return value + 1; },
                    [](const int& value) { return value + 2; });
  const auto cancelled = parallel.invoke(1, stop_source.get_token());
  require(
    cancelled.stop_reason == wuwe::fan_out_stop_reason::cancelled && cancelled.skipped_count == 2,
    "flow cancellation reaches fan_out scheduling through the shared context");
}

void fan_out_preserves_order_and_bounds_concurrency() {
  std::atomic<int> active { 0 };
  std::atomic<int> maximum { 0 };
  const auto branch = [&](int id, std::chrono::milliseconds delay) {
    return [&, id, delay](const int& input) {
      const auto current = ++active;
      auto observed = maximum.load();
      while (observed < current && !maximum.compare_exchange_weak(observed, current)) {
      }
      std::this_thread::sleep_for(delay);
      --active;
      return input + id;
    };
  };
  auto scatter = wuwe::fan_out(wuwe::fan_out_options { .max_concurrency = 2 },
    branch(0, 30ms),
    branch(1, 20ms),
    branch(2, 10ms),
    branch(3, 1ms));
  const auto result = scatter.run(10);
  require(result && result.completed_count == 4 && maximum == 2,
    "fan_out enforces the configured concurrency bound");
  for (std::size_t index = 0; index < result.items.size(); ++index) {
    require(result.items[index].index == index &&
              *result.items[index].value == 10 + static_cast<int>(index),
      "fan_out result order follows branch declaration order");
  }
}

void fan_out_and_fan_in_compose_inside_a_flow() {
  auto client = std::make_shared<test_llm_client>();
  auto pipeline = client | [](int value) { return value; } |
                  wuwe::fan_out([](const int& value) { return value + 1; },
                    [](const int& value) { return value + 2; },
                    [](const int& value) { return value + 3; }) |
                  wuwe::fan_in_all() | [](const std::vector<int>& values) {
                    int sum = 0;
                    for (const auto value : values)
                      sum += value;
                    return sum;
                  };
  require(pipeline.invoke(10) == 36, "fan_out and fan_in are first-class typed flow steps");
}

void fan_out_each_supports_runtime_sized_workloads() {
  auto parallel_map = wuwe::fan_out_each(wuwe::fan_out_options { .max_concurrency = 2 },
    [](const int& value, const wuwe::fan_out_context& context) {
      return value * value + static_cast<int>(context.index);
    });
  const auto result = parallel_map.run(std::vector<int> { 2, 3, 4, 5 });
  require(result && result.items.size() == 4 && *result.items[0].value == 4 &&
            *result.items[1].value == 10 && *result.items[2].value == 18 &&
            *result.items[3].value == 28,
    "fan_out_each maps runtime-sized random-access ranges in input order");
  const auto empty = parallel_map.run(std::vector<int> {});
  require(empty && empty.items.empty(),
    "fan_out_each treats an empty workload as a successful empty result");

  auto client = std::make_shared<test_llm_client>();
  std::vector<wuwe::llm_request> requests(2);
  requests[0].messages.push_back({ .role = "user", .content = "candidate-a" });
  requests[1].messages.push_back({ .role = "user", .content = "candidate-b" });
  auto generate = wuwe::fan_out_each(
    [client](const wuwe::llm_request& request, const wuwe::fan_out_context& context) {
      return client->complete(request, context.stop_token);
    });
  const auto candidates = generate.run(std::move(requests));
  require(candidates && candidates.items[0].value->content == "candidate-a" &&
            candidates.items[1].value->content == "candidate-b",
    "fan_out_each supports dynamic parallel LLM candidate generation");
}

void fan_out_each_reuses_a_bounded_worker_set() {
  std::mutex thread_ids_mutex;
  std::vector<std::thread::id> thread_ids;
  auto parallel_map =
    wuwe::fan_out_each(wuwe::fan_out_options { .max_concurrency = 3 }, [&](const int& value) {
      {
        std::scoped_lock lock(thread_ids_mutex);
        if (std::find(thread_ids.begin(), thread_ids.end(), std::this_thread::get_id()) ==
            thread_ids.end()) {
          thread_ids.push_back(std::this_thread::get_id());
        }
      }
      std::this_thread::sleep_for(1ms);
      return value;
    });
  std::vector<int> workload(64, 1);
  const auto result = parallel_map.run(std::move(workload));
  require(result && result.completed_count == 64 && !thread_ids.empty() && thread_ids.size() <= 3,
    "fan_out_each reuses no more than max_concurrency worker threads");
}

void collect_all_preserves_partial_failures() {
  auto scatter = wuwe::fan_out(
    wuwe::fan_out_options {
      .max_concurrency = 3,
      .failure_mode = wuwe::fan_out_failure_mode::collect_all,
    },
    [](const int& value) { return value + 1; },
    [](const int&) -> int { throw std::runtime_error("branch failed"); },
    [](const int& value) { return value + 3; });
  auto result = scatter.run(5);
  require(!result && result.stop_reason == wuwe::fan_out_stop_reason::none &&
            result.completed_count == 2 && result.failed_count == 1 && result.partial_success() &&
            result.items[1].exception && result.items[1].error == "branch failed",
    "collect_all records structured partial failures without stopping siblings");

  bool all_rejected = false;
  try {
    (void)wuwe::fan_in_all()(result);
  }
  catch (const wuwe::fan_out_error&) {
    all_rejected = true;
  }
  require(all_rejected, "fan_in_all rejects partial fan_out results explicitly");
  const auto successes = wuwe::fan_in_successes()(std::move(result));
  require(successes == std::vector<int>({ 6, 8 }),
    "fan_in_successes gathers successful values in stable order");
}

void fail_fast_returns_without_waiting_for_uncooperative_branches() {
  struct callback_state {
    std::atomic<bool> sibling_entered { false };
    std::atomic<bool> cancellation_observed { false };
    std::atomic<bool> release_sibling { false };
    std::atomic<bool> sibling_finished { false };
  };
  auto state = std::make_shared<callback_state>();
  auto scatter = wuwe::fan_out(
    wuwe::fan_out_options {
      .max_concurrency = 2,
      .failure_mode = wuwe::fan_out_failure_mode::fail_fast,
    },
    [state](const int&) -> int {
      while (!state->sibling_entered)
        std::this_thread::sleep_for(1ms);
      throw std::runtime_error("stop the group");
    },
    [state](const int&, const wuwe::fan_out_context& context) {
      std::stop_callback callback(
        context.stop_token, [state] { state->cancellation_observed = true; });
      state->sibling_entered = true;
      while (!state->release_sibling)
        std::this_thread::sleep_for(1ms);
      state->sibling_finished = true;
      return 2;
    },
    [](const int&) { return 3; });

  const auto started = std::chrono::steady_clock::now();
  const auto result = scatter.run(0);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  state->release_sibling = true;
  const auto cleanup_deadline = std::chrono::steady_clock::now() + 5s;
  while (state->sibling_entered && !state->sibling_finished &&
         std::chrono::steady_clock::now() < cleanup_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  require(result.stop_reason == wuwe::fan_out_stop_reason::fail_fast &&
            result.items[0].status == wuwe::fan_out_item_status::failed &&
            result.items[1].status == wuwe::fan_out_item_status::cancelled &&
            result.items[1].detached &&
            result.items[2].status == wuwe::fan_out_item_status::skipped &&
            state->cancellation_observed && state->sibling_finished && elapsed < 200ms,
    "fail_fast requests cancellation and detaches uncooperative active branches");
}

void timeout_and_external_cancellation_are_prompt() {
  struct timeout_state {
    std::atomic<bool> started { false };
    std::atomic<bool> release { false };
    std::atomic<bool> finished { false };
  };
  auto timed_state = std::make_shared<timeout_state>();
  auto timed = wuwe::fan_out(
    wuwe::fan_out_options {
      .max_concurrency = 1,
      .timeout = 250ms,
    },
    [timed_state](const int&, const wuwe::fan_out_context& context) {
      require(context.deadline.has_value() && context.remaining_time() >= 0ms,
        "fan_out branch receives the effective deadline");
      timed_state->started = true;
      while (!timed_state->release)
        std::this_thread::sleep_for(1ms);
      timed_state->finished = true;
      return 1;
    },
    [](const int&) { return 2; });
  const auto timed_result = timed.run(0);
  timed_state->release = true;
  const auto timed_cleanup_deadline = std::chrono::steady_clock::now() + 5s;
  while (timed_state->started && !timed_state->finished &&
         std::chrono::steady_clock::now() < timed_cleanup_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  require(timed_result.stop_reason == wuwe::fan_out_stop_reason::timed_out &&
            timed_result.items[0].status == wuwe::fan_out_item_status::timed_out &&
            timed_result.items[0].detached &&
            timed_result.items[1].status == wuwe::fan_out_item_status::skipped &&
            timed_state->started && timed_state->finished && timed_result.elapsed < 1s,
    "fan_out timeout returns without waiting for an uncooperative branch");

  struct cancellation_state {
    std::atomic<bool> entered { false };
    std::atomic<bool> release { false };
    std::atomic<bool> finished { false };
  };
  auto cancelled_state = std::make_shared<cancellation_state>();
  auto cancellable = wuwe::fan_out(
    wuwe::fan_out_options { .max_concurrency = 1 },
    [cancelled_state](const int&) {
      cancelled_state->entered = true;
      while (!cancelled_state->release)
        std::this_thread::sleep_for(1ms);
      cancelled_state->finished = true;
      return 1;
    },
    [](const int&) { return 2; });
  std::stop_source stop_source;
  std::jthread canceller([cancelled_state, &stop_source] {
    while (!cancelled_state->entered)
      std::this_thread::sleep_for(1ms);
    stop_source.request_stop();
  });
  const auto cancelled = cancellable.run(0, stop_source.get_token());
  cancelled_state->release = true;
  const auto cancelled_cleanup_deadline = std::chrono::steady_clock::now() + 5s;
  while (cancelled_state->entered && !cancelled_state->finished &&
         std::chrono::steady_clock::now() < cancelled_cleanup_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  require(cancelled.stop_reason == wuwe::fan_out_stop_reason::cancelled &&
            cancelled.items[0].status == wuwe::fan_out_item_status::cancelled &&
            cancelled.items[0].detached &&
            cancelled.items[1].status == wuwe::fan_out_item_status::skipped &&
            cancelled_state->finished,
    "external cancellation stops scheduling and reports detached work");
}

void absolute_deadline_prevents_late_branch_start() {
  std::atomic<bool> invoked { false };
  const auto result = wuwe::fan_out(
    wuwe::fan_out_options {
      .max_concurrency = 1,
      .timeout = 5s,
      .deadline = std::chrono::steady_clock::now() - 1ms,
    },
    [&](const int&) {
      invoked = true;
      return 1;
    }).run(0);
  require(result.stop_reason == wuwe::fan_out_stop_reason::timed_out && result.items.size() == 1 &&
            result.items.front().status == wuwe::fan_out_item_status::skipped && !invoked &&
            result.elapsed < 200ms,
    "an expired absolute deadline wins over a longer relative timeout without launching work");
}

void void_and_move_only_results_are_supported() {
  std::atomic<int> side_effects { 0 };
  auto void_result =
    wuwe::fan_out([&](const int&) { ++side_effects; }, [&](const int&) { ++side_effects; }).run(0);
  const auto joined_void = wuwe::fan_in_all()(std::move(void_result));
  require(side_effects == 2 && joined_void.size() == 2,
    "void fan_out branches normalize to monostate values");

  auto move_only = wuwe::fan_out([](const int& value) { return std::make_unique<int>(value + 1); },
    [](const int& value) {
      return std::make_unique<int>(value + 2);
    }).run(5);
  auto joined = wuwe::fan_in_all()(std::move(move_only));
  require(*joined[0] == 6 && *joined[1] == 7, "fan_out and fan_in preserve move-only result types");
}

void invalid_options_are_rejected() {
  bool zero_rejected = false;
  try {
    (void)wuwe::fan_out(
      wuwe::fan_out_options { .max_concurrency = 0 }, [](const int& value) { return value; });
  }
  catch (const std::invalid_argument&) {
    zero_rejected = true;
  }
  bool negative_rejected = false;
  try {
    (void)wuwe::fan_out(
      wuwe::fan_out_options { .timeout = -1ms }, [](const int& value) { return value; });
  }
  catch (const std::invalid_argument&) {
    negative_rejected = true;
  }
  require(zero_rejected && negative_rejected,
    "fan_out rejects invalid concurrency and timeout configuration");
}

void run(const char* name, void (*test)()) {
  test();
  wuwe::println("[PASS] {}", name);
}
} // namespace

int main() {
  try {
    run("flow context propagates to steps and LLM calls",
      flow_context_propagates_to_steps_and_llm_calls);
    run("fan_out preserves order and bounds concurrency",
      fan_out_preserves_order_and_bounds_concurrency);
    run("fan_out and fan_in compose inside a flow", fan_out_and_fan_in_compose_inside_a_flow);
    run("fan_out_each supports runtime-sized workloads",
      fan_out_each_supports_runtime_sized_workloads);
    run("fan_out_each reuses a bounded worker set", fan_out_each_reuses_a_bounded_worker_set);
    run("collect_all preserves partial failures", collect_all_preserves_partial_failures);
    run("fail_fast returns promptly", fail_fast_returns_without_waiting_for_uncooperative_branches);
    run("timeout and cancellation are prompt", timeout_and_external_cancellation_are_prompt);
    run("absolute deadlines prevent late work", absolute_deadline_prevents_late_branch_start);
    run("void and move-only results are supported", void_and_move_only_results_are_supported);
    run("invalid options are rejected", invalid_options_are_rejected);
  }
  catch (const std::exception& ex) {
    wuwe::println("[FAIL] {}", ex.what());
    return 1;
  }
}
