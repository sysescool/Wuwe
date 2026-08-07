#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <wuwe/agent/exploration/exploration.hpp>

namespace {

using namespace std::chrono_literals;
namespace exploration = wuwe::agent::exploration;
namespace approval = wuwe::agent::approval;
namespace capability = wuwe::agent::capability;

void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

class nonstandard_throwing_approval final : public approval::approval_service {
public:
  approval::approval_decision decide(const approval::approval_request&) override {
    throw 42;
  }
};

exploration::exploration_runner make_runner(int& executions,
  exploration::exploration_store* store = nullptr, approval::approval_service* approvals = nullptr,
  double confidence = 0.9) {
  return exploration::exploration_runner({
    .generate =
      [](const exploration::exploration_request&, const exploration::exploration_context&) {
        return std::vector<exploration::hypothesis> {
          { .statement = "Caching reduces latency", .prior_confidence = 0.6 },
        };
      },
    .design =
      [](const exploration::exploration_request&,
        const exploration::hypothesis& hypothesis,
        const exploration::exploration_context&) {
        return std::vector<exploration::experiment> {
        {
          .hypothesis_id = hypothesis.id,
          .title = "Read-only benchmark",
          .safety = exploration::experiment_safety::read_only,
        },
        {
          .hypothesis_id = hypothesis.id,
          .title = "Deploy cache configuration",
          .safety = exploration::experiment_safety::effectful,
          .capabilities = {
            {
              .name = capability::names::filesystem_write,
              .risk = capability::capability_risk_level::high,
            },
          },
        },
      };
      },
    .execute =
      [&](const exploration::experiment& experiment, const exploration::exploration_context&) {
        ++executions;
        return exploration::experiment_evidence {
          .succeeded = true,
          .observation = { { "experiment", experiment.title }, { "latency_ms", 12 } },
          .summary = "latency improved",
        };
      },
    .review =
      [confidence](const exploration::exploration_request&,
        const exploration::hypothesis&,
        const std::vector<exploration::experiment_evidence>& evidence,
        const exploration::exploration_context&) {
        return exploration::hypothesis_assessment {
          .verdict = evidence.empty() ? exploration::hypothesis_verdict::inconclusive
                                      : exploration::hypothesis_verdict::supported,
          .confidence = confidence,
          .conclusion = "benchmark supports the hypothesis",
        };
      },
    .store = store,
    .approvals = approvals,
  });
}

void blocks_effectful_experiments_by_default() {
  int executions = 0;
  exploration::in_memory_exploration_store store;
  auto runner = make_runner(executions, &store);
  const auto result = runner.run({ .objective = "reduce request latency" });

  require(result && result.hypothesis_count == 1 && result.experiment_count == 2,
    "exploration creates bounded hypotheses and experiments");
  require(executions == 1 && result.completed_experiment_count == 1 &&
            result.blocked_experiment_count == 1,
    "default policy executes read-only work and blocks effectful experiments");
  require(result.supported_count == 1 && store.load(result.record.id).has_value(),
    "evidence is reviewed and the complete exploration record is persisted");
}

void approval_enables_explicit_effectful_experiments() {
  int executions = 0;
  approval::allow_all_approval_service approvals;
  auto runner = make_runner(executions, nullptr, &approvals);
  const auto result = runner.run({ .objective = "approved exploration" },
    {
      .policy = { .allow_effectful_experiments = true },
    });

  require(result && executions == 2 && result.completed_experiment_count == 2 &&
            result.blocked_experiment_count == 0,
    "explicit policy and approval enable effectful experiments");
  const auto& effectful = result.record.hypotheses.front().experiments.back();
  require(
    effectful.approval && effectful.approval->kind == approval::approval_decision_kind::approved,
    "effectful experiment retains its approval evidence");
  require(std::any_of(effectful.specification.capabilities.begin(),
            effectful.specification.capabilities.end(),
            [](const capability::capability_request& request) {
              return request.name == capability::names::exploration_execute;
            }),
    "effectful experiments receive the generic exploration capability");
}

void low_confidence_reviews_remain_inconclusive() {
  int executions = 0;
  auto runner = make_runner(executions, nullptr, nullptr, 0.4);
  const auto result = runner.run({ .objective = "uncertain exploration" });
  require(
    result && result.supported_count == 0 && result.inconclusive_count == 1 &&
      result.record.hypotheses.front().verdict == exploration::hypothesis_verdict::inconclusive,
    "review confidence threshold prevents overclaiming conclusions");
}

void bounds_uncooperative_experiments() {
  struct callback_state {
    std::atomic<bool> started { false };
    std::atomic<bool> release { false };
    std::atomic<bool> finished { false };
  };
  auto state = std::make_shared<callback_state>();
  exploration::exploration_run_result result;
  {
    exploration::exploration_runner runner({
      .generate =
        [](const exploration::exploration_request&, const exploration::exploration_context&) {
          return std::vector<exploration::hypothesis> { { .statement = "timeout" } };
        },
      .design =
        [](const exploration::exploration_request&,
          const exploration::hypothesis& hypothesis,
          const exploration::exploration_context&) {
          return std::vector<exploration::experiment> {
            { .hypothesis_id = hypothesis.id, .title = "blocking experiment" },
          };
        },
      .execute =
        [state](const exploration::experiment&, const exploration::exploration_context&) {
          state->started = true;
          while (!state->release)
            std::this_thread::sleep_for(1ms);
          state->finished = true;
          return exploration::experiment_evidence { .succeeded = true };
        },
      .review =
        [](const exploration::exploration_request&,
          const exploration::hypothesis&,
          const std::vector<exploration::experiment_evidence>&,
          const exploration::exploration_context&) {
          return exploration::hypothesis_assessment {};
        },
    });
    result = runner.run({ .objective = "timeout" },
      {
        .policy = { .timeout = 1s },
      });
  }
  state->release = true;
  const auto cleanup_deadline = std::chrono::steady_clock::now() + 5s;
  while (
    state->started && !state->finished && std::chrono::steady_clock::now() < cleanup_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  require(!result && result.record.stop_reason == exploration::exploration_stop_reason::timed_out &&
            result.record.detached_count == 1 && result.record.hypotheses.size() == 1 &&
            result.record.hypotheses.front().experiments.size() == 1 &&
            result.record.hypotheses.front().experiments.front().detached && state->started &&
            state->finished,
    "exploration timeout reports detached uncooperative execution safely");
}

void reports_total_design_failure() {
  exploration::exploration_runner runner({
    .generate =
      [](const exploration::exploration_request&, const exploration::exploration_context&) {
        return std::vector<exploration::hypothesis> { { .statement = "design failure" } };
      },
    .design = [](const exploration::exploration_request&,
                const exploration::hypothesis&,
                const exploration::exploration_context&) -> std::vector<exploration::experiment> {
      throw std::runtime_error("designer unavailable");
    },
    .execute =
      [](const exploration::experiment&, const exploration::exploration_context&) {
        return exploration::experiment_evidence {};
      },
    .review =
      [](const exploration::exploration_request&,
        const exploration::hypothesis&,
        const std::vector<exploration::experiment_evidence>&,
        const exploration::exploration_context&) { return exploration::hypothesis_assessment {}; },
  });
  const auto result = runner.run({ .objective = "design failure" });
  require(!result &&
            result.record.stop_reason == exploration::exploration_stop_reason::design_failed &&
            result.failed_hypothesis_count == 1,
    "exploration distinguishes total designer failure from inconclusive evidence");
}

void telemetry_failures_follow_the_common_policy() {
  const auto options = [](auto observer, auto mode) {
    return exploration::exploration_runner_options {
      .generate =
        [](const exploration::exploration_request&, const exploration::exploration_context&) {
          return std::vector<exploration::hypothesis> {};
        },
      .design =
        [](const exploration::exploration_request&,
          const exploration::hypothesis&,
          const exploration::exploration_context&) {
          return std::vector<exploration::experiment> {};
        },
      .execute =
        [](const exploration::experiment&, const exploration::exploration_context&) {
          return exploration::experiment_evidence {};
        },
      .review =
        [](const exploration::exploration_request&,
          const exploration::hypothesis&,
          const std::vector<exploration::experiment_evidence>&,
          const exploration::exploration_context&) {
          return exploration::hypothesis_assessment {};
        },
      .observer = observer,
      .telemetry_failure_mode = mode,
    };
  };
  const auto throwing = [](const exploration::exploration_record&) {
    throw std::runtime_error("observer unavailable");
  };
  exploration::exploration_runner isolated(
    options(throwing, wuwe::agent::observability::telemetry_failure_mode::ignore));
  const auto result = isolated.run({ .objective = "telemetry" });
  require(result && result.telemetry_error_count == 1,
    "exploration ignores and reports telemetry failures by default");

  exploration::exploration_runner strict(
    options(throwing, wuwe::agent::observability::telemetry_failure_mode::propagate));
  bool propagated = false;
  try {
    (void)strict.run({ .objective = "strict telemetry" });
  }
  catch (const std::runtime_error&) {
    propagated = true;
  }
  require(propagated, "exploration can explicitly propagate telemetry failures");
}

void nonstandard_approval_failures_are_contained() {
  int executions = 0;
  nonstandard_throwing_approval approvals;
  auto runner = make_runner(executions, nullptr, &approvals);
  const auto result = runner.run({ .objective = "approval failure" },
    {
      .policy = { .allow_effectful_experiments = true },
    });
  const auto& effectful = result.record.hypotheses.front().experiments.back();
  require(result && executions == 1 &&
            effectful.status == exploration::experiment_status::approval_required &&
            !effectful.error.empty(),
    "exploration must convert non-standard approval failures into review-required state");
}

void run(void (*test)()) {
  test();
}

} // namespace

int main() {
  try {
    run(blocks_effectful_experiments_by_default);
    run(approval_enables_explicit_effectful_experiments);
    run(low_confidence_reviews_remain_inconclusive);
    run(bounds_uncooperative_experiments);
    run(reports_total_design_failure);
    run(telemetry_failures_follow_the_common_policy);
    run(nonstandard_approval_failures_are_contained);
  }
  catch (const std::exception& ex) {
    std::cerr << "[FAIL] " << ex.what() << '\n';
    return 1;
  }
  return 0;
}
