#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <wuwe/agent/evaluation/reasoning_evaluation.hpp>
#include <wuwe/agent/reasoning/reasoning.hpp>
#include <wuwe/common/print.h>

namespace {
using namespace std::chrono_literals;
namespace reasoning = wuwe::agent::reasoning;
namespace evaluation = wuwe::agent::evaluation;

void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

reasoning::reasoning_result successful_result(
  std::string content, double cost = 0.0, std::size_t tokens = 0) {
  reasoning::reasoning_result result;
  result.completed = true;
  result.content = content;
  result.final_response.content = content;
  result.usage.model_calls = 1;
  result.usage.total_tokens = tokens;
  result.usage.completion_tokens = tokens;
  result.usage.cost_usd = cost;
  return result;
}

class temperature_llm_client final : public wuwe::llm_client {
public:
  wuwe::llm_response complete(const wuwe::llm_request& request) override {
    return {
      .content = std::to_string(request.temperature),
      .usage = { .prompt_tokens = 2, .completion_tokens = 1, .total_tokens = 3 },
    };
  }
};

void selects_the_highest_scoring_candidate_with_bounded_concurrency() {
  std::atomic<int> active { 0 };
  std::atomic<int> maximum { 0 };
  std::atomic<int> observer_active { 0 };
  std::atomic<bool> observer_overlap { false };
  reasoning::best_of_n_runner runner({
    .generator =
      [&](
        const reasoning::reasoning_request& request, const reasoning::best_of_n_context& context) {
        require(context.deadline.has_value(), "candidate receives the operation deadline");
        const auto current = ++active;
        auto observed = maximum.load();
        while (observed < current && !maximum.compare_exchange_weak(observed, current)) {
        }
        std::this_thread::sleep_for(
          std::chrono::milliseconds(4 * (4 - static_cast<int>(context.index))));
        --active;
        return successful_result(request.metadata.at("variant"),
          0.01 * static_cast<double>(context.index + 1),
          10 + context.index);
      },
    .scorer =
      [](const reasoning::reasoning_request&,
        const reasoning::reasoning_result& result,
        const reasoning::best_of_n_context&) {
        return reasoning::best_of_n_score {
          .value = std::stod(result.content),
          .rationale = "deterministic test score",
        };
      },
    .request_builder =
      [](const reasoning::reasoning_request& base, std::size_t index) {
        auto request = base;
        request.temperature = 0.1 + 0.1 * static_cast<double>(index);
        request.metadata["variant"] = std::to_string(index);
        return request;
      },
    .observer =
      [&](const reasoning::best_of_n_event&) {
        if (observer_active.fetch_add(1) != 0) {
          observer_overlap = true;
        }
        std::this_thread::sleep_for(1ms);
        --observer_active;
      },
  });

  reasoning::reasoning_request request { .input = "choose the best candidate" };
  const auto result = runner.run(std::move(request), {
    .policy = {
      .candidate_count = 4,
      .max_concurrency = 2,
      .timeout = 1s,
    },
  });

  require(result && result.selected_index == 3 && result.eligible_count == 4,
    "best-of-n selects the highest scoring eligible candidate");
  require(result.selected_candidate() && result.selected_candidate()->result.content == "3",
    "selected_candidate exposes the selected reasoning result");
  require(maximum > 0 && maximum <= 2, "candidate generation does not exceed max_concurrency");
  require(!observer_overlap, "best-of-n observer calls are serialized");
  require(result.aggregate_usage.model_calls == 4 && result.aggregate_usage.total_tokens == 46,
    "candidate usage is aggregated across the run");
  require(result.candidates.size() == 4 && result.candidates[0].index == 0 &&
            result.candidates[3].index == 3,
    "candidate results preserve request order");
  for (std::size_t index = 0; index < result.trace.size(); ++index) {
    require(
      result.trace[index].sequence == index, "best-of-n trace sequence is stable and contiguous");
  }
  const auto json = reasoning::best_of_n_result_to_json(result);
  require(json["selected_index"] == 3 && json["candidates"].size() == 4,
    "best-of-n results have a structured JSON representation");
}

void preserves_generation_scoring_and_rejection_failures() {
  reasoning::best_of_n_runner runner({
    .generator =
      [](const reasoning::reasoning_request& request, const reasoning::best_of_n_context& context) {
        if (context.index == 1) {
          throw std::runtime_error("generator failed");
        }
        if (context.index == 2) {
          auto result = successful_result("incomplete");
          result.completed = false;
          result.error = "model did not complete";
          return result;
        }
        return successful_result(request.metadata.at("variant"));
      },
    .scorer =
      [](const reasoning::reasoning_request&,
        const reasoning::reasoning_result& result,
        const reasoning::best_of_n_context& context) {
        if (context.index == 3) {
          throw std::runtime_error("scorer failed");
        }
        return reasoning::best_of_n_score {
          .value = context.index == 0 ? 0.8 : 0.9,
          .accepted = result.content != "4",
        };
      },
    .request_builder =
      [](const reasoning::reasoning_request& base, std::size_t index) {
        if (index == 5) {
          throw std::runtime_error("builder failed");
        }
        auto request = base;
        request.metadata["variant"] = std::to_string(index);
        return request;
      },
  });

  const auto result = runner.run({ .input = "partial candidates" },
    {
      .policy = { .candidate_count = 6, .max_concurrency = 3 },
    });
  require(result && result.selected_index == 0 && result.eligible_count == 1 &&
            result.rejected_count == 1 && result.failed_count == 4,
    "best-of-n selects from partial success while preserving structured failures");
  require(result.candidates[1].status == reasoning::best_of_n_candidate_status::generation_failed &&
            result.candidates[3].status == reasoning::best_of_n_candidate_status::scoring_failed &&
            result.candidates[4].status == reasoning::best_of_n_candidate_status::rejected &&
            result.candidates[5].error == "builder failed",
    "generation, scoring, rejection, and builder failures remain distinguishable");
  require(result.aggregate_usage.model_calls == 4,
    "usage includes completed and failed semantic candidates that reached a model");
}

void applies_deterministic_ties_thresholds_and_custom_selection() {
  const auto generator = [](const reasoning::reasoning_request&,
                           const reasoning::best_of_n_context& context) {
    const std::vector<double> costs { 0.2, 0.1, 0.1 };
    const std::vector<std::size_t> tokens { 10, 20, 5 };
    return successful_result(
      std::to_string(context.index), costs[context.index], tokens[context.index]);
  };
  const auto scorer = [](const reasoning::reasoning_request&,
                        const reasoning::reasoning_result&,
                        const reasoning::best_of_n_context&) {
    return reasoning::best_of_n_score { .value = 0.9 };
  };

  reasoning::best_of_n_runner runner({ .generator = generator, .scorer = scorer });
  const auto economical = runner.run({ .input = "tie" });
  require(economical.selected_index == 2,
    "score ties prefer lower cost, then fewer tokens, then stable index");

  const auto stable = runner.run({ .input = "tie" },
    {
      .policy = { .candidate_count = 3, .prefer_lower_cost_on_tie = false },
    });
  require(
    stable.selected_index == 0, "disabling cost tie-breaking preserves the first stable candidate");

  reasoning::best_of_n_runner custom({
    .generator = generator,
    .scorer = scorer,
    .selector =
      [](const std::vector<reasoning::best_of_n_candidate>&) {
        return std::optional<std::size_t>(1);
      },
  });
  require(custom.run({ .input = "custom selector" }).selected_index == 1,
    "custom selectors support voting and domain-specific aggregation");

  reasoning::best_of_n_runner majority({
    .generator =
      [](const reasoning::reasoning_request&, const reasoning::best_of_n_context& context) {
        const std::string answers[] { "A", "B", "A" };
        return successful_result(answers[context.index]);
      },
    .scorer =
      [](const reasoning::reasoning_request&,
        const reasoning::reasoning_result&,
        const reasoning::best_of_n_context& context) {
        return reasoning::best_of_n_score {
          .value = context.index == 2 ? 0.9 : 0.8,
        };
      },
    .selector = reasoning::make_majority_vote_selector(
      [](const reasoning::best_of_n_candidate& candidate) { return candidate.result.content; }),
  });
  require(majority.run({ .input = "self consistency" }).selected_index == 2,
    "majority voting selects the best-scored member of the consensus group");

  const auto rejected = runner.run({ .input = "threshold" },
    {
      .policy = { .candidate_count = 3, .minimum_score = 0.95 },
    });
  require(!rejected &&
            rejected.stop_reason == reasoning::best_of_n_stop_reason::no_eligible_candidate &&
            rejected.rejected_count == 3,
    "minimum score rejects weak candidates without losing diagnostics");
}

void timeout_and_cancellation_return_promptly_and_keep_detached_state_alive() {
  std::atomic<bool> release { false };
  std::atomic<bool> finished { false };
  reasoning::best_of_n_result timed;
  {
    reasoning::best_of_n_runner runner({
      .generator =
        [&](const reasoning::reasoning_request&, const reasoning::best_of_n_context&) {
          while (!release)
            std::this_thread::sleep_for(1ms);
          finished = true;
          return successful_result("late");
        },
      .scorer =
        [](const reasoning::reasoning_request&,
          const reasoning::reasoning_result&,
          const reasoning::best_of_n_context&) {
          return reasoning::best_of_n_score { .value = 1.0 };
        },
    });
    const auto started = std::chrono::steady_clock::now();
    timed = runner.run({ .input = "timeout" }, {
      .policy = {
        .candidate_count = 2,
        .max_concurrency = 1,
        .timeout = 20ms,
        .budget = {
          .estimated_model_calls_per_candidate = 1,
          .estimated_total_tokens_per_candidate = 50,
          .estimated_cost_usd_per_candidate = 0.25,
        },
      },
    });
    require(std::chrono::steady_clock::now() - started < 200ms,
      "best-of-n timeout does not wait for an uncooperative generator");
  }
  require(!timed && timed.stop_reason == reasoning::best_of_n_stop_reason::timed_out &&
            timed.candidates[0].status == reasoning::best_of_n_candidate_status::timed_out &&
            timed.candidates[0].detached &&
            timed.candidates[1].status == reasoning::best_of_n_candidate_status::skipped &&
            timed.outstanding_reserved_usage.model_calls == 1 &&
            timed.outstanding_reserved_usage.total_tokens == 50 &&
            timed.outstanding_reserved_usage.cost_usd == 0.25 &&
            timed.budget_accounted_usage.model_calls == 1,
    "timeout reports detached and unscheduled candidates explicitly");
  release = true;
  while (!finished)
    std::this_thread::sleep_for(1ms);

  std::stop_source stop_source;
  stop_source.request_stop();
  reasoning::best_of_n_runner cancelled_runner({
    .generator =
      [](const reasoning::reasoning_request&, const reasoning::best_of_n_context&) {
        return successful_result("must not run");
      },
    .scorer =
      [](const reasoning::reasoning_request&,
        const reasoning::reasoning_result&,
        const reasoning::best_of_n_context&) {
        return reasoning::best_of_n_score { .value = 1.0 };
      },
  });
  const auto cancelled = cancelled_runner.run({ .input = "cancel" },
    {
      .policy = { .candidate_count = 2 },
      .stop_token = stop_source.get_token(),
    });
  require(cancelled.stop_reason == reasoning::best_of_n_stop_reason::cancelled &&
            cancelled.skipped_count == 2,
    "pre-requested cancellation prevents candidate generation");

  std::atomic<bool> async_entered { false };
  reasoning::best_of_n_runner async_runner({
    .generator =
      [&](const reasoning::reasoning_request&, const reasoning::best_of_n_context& context) {
        async_entered = true;
        while (!context.cancellation_requested()) {
          std::this_thread::sleep_for(1ms);
        }
        return successful_result("cancelled candidate");
      },
    .scorer =
      [](const reasoning::reasoning_request&,
        const reasoning::reasoning_result&,
        const reasoning::best_of_n_context&) {
        return reasoning::best_of_n_score { .value = 1.0 };
      },
  });
  auto asynchronous = async_runner.run_async({ .input = "async cancellation" });
  while (!async_entered)
    std::this_thread::sleep_for(1ms);
  asynchronous.request_stop();
  const auto async_cancelled = asynchronous.get();
  require(async_cancelled.stop_reason == reasoning::best_of_n_stop_reason::cancelled &&
            asynchronous.stop_requested(),
    "run_async combines caller and worker cancellation tokens");
}

void evaluation_adapter_scores_candidates_and_exports_trajectory() {
  auto evaluator = std::make_shared<evaluation::function_evaluator>(
    "length", [](const evaluation::evaluation_case& value) {
      const auto score = static_cast<double>(value.output.size()) / 10.0;
      return evaluation::evaluation_metric_result {
        .score = score,
        .passed = score >= 0.5,
        .explanation = "prefer sufficiently complete answers",
      };
    });
  reasoning::best_of_n_runner runner({
    .generator =
      [](const reasoning::reasoning_request&, const reasoning::best_of_n_context& context) {
        return successful_result(context.index == 0 ? "tiny" : "long-answer");
      },
    .scorer = evaluation::make_candidate_evaluator_scorer(evaluator),
  });
  const auto result = runner.run({ .input = "evaluate candidates" },
    {
      .policy = { .candidate_count = 2 },
    });
  require(result && result.selected_index == 1 && result.rejected_count == 1,
    "unified evaluators can score and gate best-of-n candidates");
  const auto evaluation_case =
    evaluation::evaluation_case_from_best_of_n("best-of-n-case", "evaluate candidates", result);
  require(evaluation_case.output == "long-answer" && evaluation_case.trajectory.is_array() &&
            !evaluation_case.trajectory.empty(),
    "best-of-n results integrate with trajectory regression cases");
}

void reasoning_runner_adapter_executes_real_reasoning_candidates() {
  temperature_llm_client client;
  reasoning::reasoning_runner base_runner(client);
  reasoning::best_of_n_runner runner({
    .generator = reasoning::make_reasoning_candidate_generator(base_runner),
    .scorer =
      [](const reasoning::reasoning_request&,
        const reasoning::reasoning_result& result,
        const reasoning::best_of_n_context&) {
        return reasoning::best_of_n_score { .value = std::stod(result.content) };
      },
    .request_builder =
      [](const reasoning::reasoning_request& base, std::size_t index) {
        auto request = base;
        request.temperature = 0.2 + 0.3 * static_cast<double>(index);
        return request;
      },
  });
  reasoning::reasoning_request request {
    .input = "generate candidates",
    .policy = { .mode = reasoning::reasoning_mode::simple },
  };
  const auto result = runner.run(std::move(request),
    {
      .policy = { .candidate_count = 2, .max_concurrency = 1 },
    });
  require(result && result.selected_index == 1 && result.aggregate_usage.model_calls == 2 &&
            result.aggregate_usage.total_tokens == 6,
    "reasoning_runner adapter preserves execution and usage accounting");
}

void isolates_candidate_side_effects_and_commits_only_the_selected_result() {
  wuwe::agent::memory::memory_policy memory_policy;
  memory_policy.require_scoped_recall = false;
  wuwe::agent::memory::memory_context memory(memory_policy);
  temperature_llm_client client;
  reasoning::reasoning_runner base_runner({
    .client = &client,
    .memory = &memory,
  });
  reasoning::best_of_n_runner runner({
    .generator = reasoning::make_reasoning_candidate_generator(base_runner),
    .scorer =
      [](const reasoning::reasoning_request&,
        const reasoning::reasoning_result& result,
        const reasoning::best_of_n_context&) {
        return reasoning::best_of_n_score { .value = std::stod(result.content) };
      },
    .request_builder =
      [](const reasoning::reasoning_request& base, std::size_t index) {
        auto request = base;
        request.temperature = 0.2 + 0.3 * static_cast<double>(index);
        return request;
      },
  });
  reasoning::reasoning_request request {
    .input = "isolated candidates",
    .policy = { .mode = reasoning::reasoning_mode::simple },
  };
  const auto result = runner.run(std::move(request),
    {
      .policy = { .candidate_count = 2, .max_concurrency = 1 },
    });
  require(result && memory.list().empty(),
    "isolated candidate generation does not persist unselected responses");
  require(reasoning::commit_best_of_n_result(base_runner, result) && memory.list().size() == 1 &&
            memory.list().front().content == result.selected_candidate()->result.content,
    "only the selected candidate is explicitly committed to memory");

  std::atomic<int> plan_steps { 0 };
  auto planner = std::make_shared<wuwe::agent::planning::static_planner>(
    std::vector<wuwe::agent::planning::plan_step> {
      { .id = "step", .title = "side effect" },
    });
  auto executor = std::make_shared<wuwe::agent::planning::function_plan_executor>(
    [&](const wuwe::agent::planning::plan_step&,
      const wuwe::agent::planning::plan_execution_context&) {
      ++plan_steps;
      return wuwe::agent::planning::plan_step_result::completed("executed");
    });
  reasoning::reasoning_runner planning_runner({
    .planner = planner,
    .executor = executor,
  });
  reasoning::best_of_n_runner isolated_plan({
    .generator = reasoning::make_reasoning_candidate_generator(planning_runner),
    .scorer =
      [](const reasoning::reasoning_request&,
        const reasoning::reasoning_result&,
        const reasoning::best_of_n_context&) {
        return reasoning::best_of_n_score { .value = 1.0 };
      },
  });
  const auto blocked = isolated_plan.run(
    {
      .input = "do not execute this plan",
      .policy = { .mode = reasoning::reasoning_mode::plan_execute },
    },
    {
      .policy = { .candidate_count = 1 },
    });
  require(
    !blocked && plan_steps == 0 && blocked.side_effect_blocked_count == 1 &&
      blocked.candidates[0].status == reasoning::best_of_n_candidate_status::side_effect_blocked,
    "isolated Best-of-N rejects plan execution before side effects occur");
  const auto allowed = isolated_plan.run({
    .input = "explicitly allow plan execution",
    .policy = { .mode = reasoning::reasoning_mode::plan_execute },
  }, {
    .policy = {
      .candidate_count = 1,
      .side_effects = reasoning::best_of_n_side_effect_policy::allow,
    },
  });
  require(allowed && plan_steps == 1,
    "side-effectful candidate execution requires an explicit policy opt-in");
}

void enforces_shared_aggregate_budgets_before_additional_candidates_run() {
  std::atomic<int> generator_calls { 0 };
  reasoning::best_of_n_runner runner({
    .generator =
      [&](const reasoning::reasoning_request&, const reasoning::best_of_n_context& context) {
        ++generator_calls;
        auto result = successful_result(std::to_string(context.index));
        result.usage.model_calls = 2;
        return result;
      },
    .scorer =
      [](const reasoning::reasoning_request&,
        const reasoning::reasoning_result&,
        const reasoning::best_of_n_context& context) {
        return reasoning::best_of_n_score {
          .value = static_cast<double>(context.index),
        };
      },
  });
  const auto result = runner.run({ .input = "bounded candidates" }, {
    .policy = {
      .candidate_count = 3,
      .max_concurrency = 1,
      .budget = {
        .max_model_calls = 3,
        .estimated_model_calls_per_candidate = 1,
      },
    },
  });
  require(!result && result.stop_reason == reasoning::best_of_n_stop_reason::budget_exceeded &&
            result.budget_exceeded_count == 2 && generator_calls == 2 &&
            result.aggregate_usage.model_calls == 4,
    "shared budget blocks unscheduled work and reports actual aggregate usage");

  bool preflight_rejected = false;
  try {
    (void)runner.run({ .input = "invalid budget" }, {
      .policy = {
        .candidate_count = 3,
        .budget = {
          .max_total_tokens = 100,
          .estimated_total_tokens_per_candidate = 40,
        },
      },
    });
  }
  catch (const std::invalid_argument&) {
    preflight_rejected = true;
  }
  require(preflight_rejected, "aggregate budget preflight rejects candidate sets that cannot fit");

  std::atomic<int> scored_candidates { 0 };
  reasoning::best_of_n_runner scored_runner({
    .generator =
      [](const reasoning::reasoning_request&, const reasoning::best_of_n_context& context) {
        auto result = successful_result(std::to_string(context.index));
        result.usage.model_calls = 2;
        return result;
      },
    .scorer =
      [&](const reasoning::reasoning_request&,
        const reasoning::reasoning_result&,
        const reasoning::best_of_n_context& context) {
        ++scored_candidates;
        return reasoning::best_of_n_score {
          .value = static_cast<double>(context.index),
          .usage = { .model_calls = 1 },
        };
      },
  });
  const auto scored_result = scored_runner.run({ .input = "budget scoring" }, {
    .policy = {
      .candidate_count = 2,
      .max_concurrency = 1,
      .budget = {
        .max_model_calls = 5,
        .estimated_model_calls_per_candidate = 1,
        .estimated_scorer_model_calls_per_candidate = 1,
      },
    },
  });
  require(!scored_result &&
            scored_result.stop_reason == reasoning::best_of_n_stop_reason::budget_exceeded &&
            scored_result.budget_exceeded_count == 1 && scored_candidates == 1 &&
            scored_result.aggregate_usage.model_calls == 5,
    "scorer usage is reserved before invocation and included in aggregate accounting");
}

void bounds_request_builder_and_selector_callbacks() {
  std::atomic<bool> builder_release { false };
  std::atomic<bool> builder_finished { false };
  reasoning::best_of_n_result builder_timeout;
  {
    reasoning::best_of_n_runner runner({
      .generator = [](const reasoning::reasoning_request&,
                     const reasoning::best_of_n_context&) { return successful_result("unused"); },
      .scorer =
        [](const reasoning::reasoning_request&,
          const reasoning::reasoning_result&,
          const reasoning::best_of_n_context&) {
          return reasoning::best_of_n_score { .value = 1.0 };
        },
      .contextual_request_builder =
        [&](const reasoning::reasoning_request& base,
          std::size_t,
          const reasoning::best_of_n_context&) {
          while (!builder_release)
            std::this_thread::sleep_for(1ms);
          builder_finished = true;
          return base;
        },
    });
    builder_timeout = runner.run({ .input = "builder timeout" },
      {
        .policy = { .candidate_count = 2, .timeout = 20ms },
      });
  }
  require(
    builder_timeout.stop_reason == reasoning::best_of_n_stop_reason::timed_out &&
      builder_timeout.timed_out_count == 1 && builder_timeout.detached_count == 1 &&
      builder_timeout.skipped_count == 1 &&
      builder_timeout.candidates[0].status == reasoning::best_of_n_candidate_status::timed_out,
    "request builder is covered by the overall timeout and detached safely");
  builder_release = true;
  while (!builder_finished)
    std::this_thread::sleep_for(1ms);

  std::atomic<bool> selector_release { false };
  std::atomic<bool> selector_finished { false };
  reasoning::best_of_n_result selector_timeout;
  {
    reasoning::best_of_n_runner runner({
      .generator =
        [](const reasoning::reasoning_request&, const reasoning::best_of_n_context&) {
          return successful_result("candidate");
        },
      .scorer =
        [](const reasoning::reasoning_request&,
          const reasoning::reasoning_result&,
          const reasoning::best_of_n_context&) {
          return reasoning::best_of_n_score { .value = 1.0 };
        },
      .contextual_selector =
        [&](
          const std::vector<reasoning::best_of_n_candidate>&, const reasoning::best_of_n_context&) {
          while (!selector_release)
            std::this_thread::sleep_for(1ms);
          selector_finished = true;
          return std::optional<std::size_t>(0);
        },
    });
    selector_timeout = runner.run({ .input = "selector timeout" },
      {
        .policy = { .candidate_count = 1, .timeout = 20ms },
      });
  }
  require(!selector_timeout &&
            selector_timeout.stop_reason == reasoning::best_of_n_stop_reason::timed_out &&
            selector_timeout.coordination_detached_count == 1,
    "custom selector is covered by the overall timeout and detached safely");
  selector_release = true;
  while (!selector_finished)
    std::this_thread::sleep_for(1ms);
}

void invalid_configuration_and_selector_results_are_rejected() {
  bool missing_generator = false;
  try {
    (void)reasoning::best_of_n_runner({
      .scorer = [](const reasoning::reasoning_request&,
                  const reasoning::reasoning_result&,
                  const reasoning::best_of_n_context&) { return reasoning::best_of_n_score {}; },
    });
  }
  catch (const std::invalid_argument&) {
    missing_generator = true;
  }

  reasoning::best_of_n_runner runner({
    .generator = [](const reasoning::reasoning_request&,
                   const reasoning::best_of_n_context&) { return successful_result("candidate"); },
    .scorer =
      [](const reasoning::reasoning_request&,
        const reasoning::reasoning_result&,
        const reasoning::best_of_n_context&) {
        return reasoning::best_of_n_score { .value = 1.0 };
      },
  });
  bool zero_candidates = false;
  try {
    (void)runner.run({ .input = "invalid" },
      {
        .policy = { .candidate_count = 0 },
      });
  }
  catch (const std::invalid_argument&) {
    zero_candidates = true;
  }

  reasoning::best_of_n_runner invalid_selector({
    .generator = [](const reasoning::reasoning_request&,
                   const reasoning::best_of_n_context&) { return successful_result("candidate"); },
    .scorer =
      [](const reasoning::reasoning_request&,
        const reasoning::reasoning_result&,
        const reasoning::best_of_n_context&) {
        return reasoning::best_of_n_score { .value = 1.0 };
      },
    .selector =
      [](const std::vector<reasoning::best_of_n_candidate>&) {
        return std::optional<std::size_t>(99);
      },
  });
  const auto selection = invalid_selector.run({ .input = "invalid selection" });
  require(missing_generator && zero_candidates && !selection &&
            selection.stop_reason == reasoning::best_of_n_stop_reason::selection_failed,
    "invalid runner configuration and selector output fail explicitly");
}

void telemetry_failures_follow_the_common_policy() {
  auto generator = [](const reasoning::reasoning_request&, const reasoning::best_of_n_context&) {
    return successful_result("candidate");
  };
  auto scorer = [](const reasoning::reasoning_request&,
                  const reasoning::reasoning_result&,
                  const reasoning::best_of_n_context&) {
    return reasoning::best_of_n_score { .value = 1.0 };
  };
  reasoning::best_of_n_runner isolated({
    .generator = generator,
    .scorer = scorer,
    .observer =
      [](const reasoning::best_of_n_event&) { throw std::runtime_error("telemetry unavailable"); },
  });
  const auto result = isolated.run({ .input = "telemetry" },
    {
      .policy = { .candidate_count = 1 },
    });
  require(result && result.telemetry_error_count != 0,
    "best-of-n ignores and reports telemetry failures by default");

  reasoning::best_of_n_runner terminal_only({
    .generator = generator,
    .scorer = scorer,
    .observer =
      [](const reasoning::best_of_n_event& event) {
        if (event.type == reasoning::best_of_n_event_type::cancelled) {
          throw std::runtime_error("terminal telemetry unavailable");
        }
      },
  });
  std::stop_source cancelled;
  cancelled.request_stop();
  const auto cancelled_result = terminal_only.run({ .input = "cancelled telemetry" },
    {
      .policy = { .candidate_count = 1 },
      .stop_token = cancelled.get_token(),
    });
  require(!cancelled_result && cancelled_result.telemetry_error_count == 1,
    "best-of-n accounts for terminal telemetry failures on unsuccessful runs");

  reasoning::best_of_n_runner strict({
    .generator = generator,
    .scorer = scorer,
    .observer =
      [](const reasoning::best_of_n_event&) { throw std::runtime_error("telemetry unavailable"); },
    .telemetry_failure_mode = wuwe::agent::observability::telemetry_failure_mode::propagate,
  });
  bool propagated = false;
  try {
    (void)strict.run({ .input = "strict telemetry" },
      {
        .policy = { .candidate_count = 1 },
      });
  }
  catch (const std::runtime_error&) {
    propagated = true;
  }
  require(propagated, "best-of-n can explicitly propagate telemetry failures");
}

void run(const char* name, void (*test)()) {
  test();
  wuwe::println("[PASS] {}", name);
}
} // namespace

int main() {
  try {
    run("selects highest score with bounded concurrency",
      selects_the_highest_scoring_candidate_with_bounded_concurrency);
    run("preserves candidate failures", preserves_generation_scoring_and_rejection_failures);
    run("applies ties thresholds and custom selection",
      applies_deterministic_ties_thresholds_and_custom_selection);
    run("handles timeout and cancellation",
      timeout_and_cancellation_return_promptly_and_keep_detached_state_alive);
    run(
      "integrates unified evaluation", evaluation_adapter_scores_candidates_and_exports_trajectory);
    run("adapts reasoning runner candidates",
      reasoning_runner_adapter_executes_real_reasoning_candidates);
    run("isolates and commits candidate side effects",
      isolates_candidate_side_effects_and_commits_only_the_selected_result);
    run("enforces aggregate budgets",
      enforces_shared_aggregate_budgets_before_additional_candidates_run);
    run("bounds builder and selector callbacks", bounds_request_builder_and_selector_callbacks);
    run("rejects invalid configuration", invalid_configuration_and_selector_results_are_rejected);
    run("uses common telemetry failure policy", telemetry_failures_follow_the_common_policy);
  }
  catch (const std::exception& ex) {
    wuwe::println("[FAIL] {}", ex.what());
    return 1;
  }
}
