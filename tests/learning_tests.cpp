#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

#include <wuwe/agent/learning/learning.hpp>
#include <wuwe/agent/learning/exploration_adapter.hpp>

namespace {

using namespace std::chrono_literals;
namespace learning = wuwe::agent::learning;
namespace evaluation = wuwe::agent::evaluation;
namespace approval = wuwe::agent::approval;
namespace exploration = wuwe::agent::exploration;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

class nonstandard_throwing_approval final : public approval::approval_service {
public:
  approval::approval_decision decide(
    const approval::approval_request&) override {
    throw 42;
  }
};

learning::learning_evaluation evaluation_for(double score, std::size_t regressions = 0) {
  return {
    .passed = true,
    .baseline_score = 0.7,
    .candidate_score = score,
    .baseline_pass_rate = 0.8,
    .candidate_pass_rate = score,
    .regression_count = regressions,
  };
}

void stages_only_candidates_that_pass_promotion_policy() {
  learning::in_memory_learning_store store;
  int activations = 0;
  learning::learning_runner runner({
    .proposer = [](const learning::learning_request&,
                  const learning::learning_context&) {
      return std::vector<learning::learning_candidate> {
        {
          .kind = learning::learning_change_kind::prompt,
          .proposed_version = "prompt-v2",
          .artifact = { { "template", "improved" }, { "score", 0.9 } },
        },
        {
          .kind = learning::learning_change_kind::prompt,
          .proposed_version = "prompt-v3",
          .artifact = { { "template", "regressed" }, { "score", 0.6 } },
        },
      };
    },
    .evaluator = [](const learning::learning_candidate& candidate,
                    const learning::learning_context&) {
      return evaluation_for(candidate.artifact.at("score").get<double>());
    },
    .activator = [&](const learning::learning_candidate&,
                     const learning::learning_context&) {
      ++activations;
      return learning::learning_activation_result { .activated = true };
    },
    .store = &store,
  });

  const auto result = runner.run({
    .goal = "improve response quality",
    .target = "support.prompt",
    .baseline_version = "prompt-v1",
  }, {
    .policy = {
      .minimum_candidate_score = 0.8,
      .minimum_candidate_pass_rate = 0.8,
      .minimum_score_improvement = 0.05,
    },
  });

  require(result && result.proposed_count == 2 && result.evaluated_count == 2,
    "learning runner evaluates every bounded proposal");
  require(result.accepted_count == 1 && result.rejected_count == 1,
    "promotion policy separates accepted candidates from regressions");
  require(result.activated_count == 0 && activations == 0,
    "stage_only is the safe default and never invokes activation");
  require(store.list().size() == 2,
    "accepted and rejected learning evidence is auditable by default");
}

void activates_only_after_explicit_approval() {
  approval::allow_all_approval_service approvals;
  learning::in_memory_learning_store store;
  int activations = 0;
  learning::learning_runner runner({
    .proposer = [](const learning::learning_request&,
                  const learning::learning_context&) {
      return std::vector<learning::learning_candidate> {
        {
          .kind = learning::learning_change_kind::reasoning_policy,
          .proposed_version = "policy-v2",
          .artifact = { { "max_steps", 6 } },
        },
      };
    },
    .evaluator = [](const learning::learning_candidate&,
                    const learning::learning_context&) {
      return evaluation_for(0.92);
    },
    .activator = [&](const learning::learning_candidate& candidate,
                     const learning::learning_context&) {
      ++activations;
      return learning::learning_activation_result {
        .activated = true,
        .active_version = candidate.proposed_version,
        .previous_version = candidate.parent_version,
        .rollback_token = "rollback-policy-v1",
      };
    },
    .store = &store,
    .approvals = &approvals,
  });

  const auto result = runner.run({
    .target = "reasoning.default-policy",
    .baseline_version = "policy-v1",
  }, {
    .policy = {
      .activation_mode = learning::learning_activation_mode::require_approval,
    },
  });
  require(result && result.activated_count == 1 && activations == 1,
    "approved learning candidate is activated exactly once");
  require(result.records.front().approval && result.records.front().activation &&
      result.records.front().activation->rollback_token == "rollback-policy-v1",
    "activation record preserves approval and rollback metadata");

  approval::deny_all_approval_service denied;
  learning::learning_runner denied_runner({
    .proposer = [](const learning::learning_request&,
                  const learning::learning_context&) {
      return std::vector<learning::learning_candidate> {
        { .proposed_version = "denied-v1" },
      };
    },
    .evaluator = [](const learning::learning_candidate&,
                    const learning::learning_context&) {
      return evaluation_for(0.95);
    },
    .activator = [&](const learning::learning_candidate&,
                     const learning::learning_context&) {
      ++activations;
      return learning::learning_activation_result { .activated = true };
    },
    .approvals = &denied,
  });
  const auto denied_result = denied_runner.run({ .target = "denied" }, {
    .policy = {
      .activation_mode = learning::learning_activation_mode::require_approval,
    },
  });
  require(denied_result && denied_result.approval_denied_count == 1 &&
      denied_result.accepted_count == 1 && activations == 1,
    "denied approval prevents activation without failing the controlled run");
}

void nonstandard_approval_failures_are_contained() {
  nonstandard_throwing_approval approvals;
  learning::learning_runner runner({
    .proposer = [](const learning::learning_request&,
                   const learning::learning_context&) {
      return std::vector<learning::learning_candidate> {
        { .proposed_version = "approval-failure-v1" },
      };
    },
    .evaluator = [](const learning::learning_candidate&,
                    const learning::learning_context&) {
      return evaluation_for(0.95);
    },
    .activator = [](const learning::learning_candidate&,
                    const learning::learning_context&) {
      return learning::learning_activation_result { .activated = true };
    },
    .approvals = &approvals,
  });
  const auto result = runner.run({ .target = "approval-failure" }, {
    .policy = {
      .activation_mode = learning::learning_activation_mode::require_approval,
    },
  });
  require(result && result.approval_required_count == 1 &&
      result.records.front().status ==
        learning::learning_candidate_status::approval_required,
    "learning must convert non-standard approval failures into review-required state");
}

void evaluation_suite_comparison_detects_regressions() {
  evaluation::evaluation_suite_result baseline;
  baseline.total = 2;
  baseline.passed = 2;
  baseline.pass_rate = 1.0;
  baseline.mean_score = 0.8;
  baseline.cases = {
    { .id = "stable", .passed = true, .score = 0.8 },
    { .id = "regressed", .passed = true, .score = 0.8 },
  };
  evaluation::evaluation_suite_result candidate = baseline;
  candidate.passed = 1;
  candidate.failed = 1;
  candidate.pass_rate = 0.5;
  candidate.mean_score = 0.85;
  candidate.cases[1].passed = false;

  const auto compared = learning::compare_evaluation_suites(baseline, candidate);
  require(compared.regression_count == 1 &&
      compared.regressions.front() == "regressed" &&
      compared.score_improvement() > 0.0 &&
      compared.pass_rate_improvement() < 0.0,
    "suite comparison distinguishes aggregate gains from case regressions");
}

void bounds_uncooperative_evaluators() {
  std::atomic<bool> release { false };
  std::atomic<bool> finished { false };
  learning::learning_run_result result;
  {
    learning::learning_runner runner({
      .proposer = [](const learning::learning_request&,
                    const learning::learning_context&) {
        return std::vector<learning::learning_candidate> { { .target = "timeout" } };
      },
      .evaluator = [&](const learning::learning_candidate&,
                       const learning::learning_context&) {
        while (!release) std::this_thread::sleep_for(1ms);
        finished = true;
        return evaluation_for(0.9);
      },
    });
    result = runner.run({ .target = "timeout" }, {
      .policy = { .timeout = 20ms },
    });
  }
  require(!result && result.stop_reason == learning::learning_stop_reason::timed_out &&
      result.detached_count == 1 && result.records.front().detached,
    "learning timeout returns without destroying detached callback ownership");
  release = true;
  while (!finished) std::this_thread::sleep_for(1ms);
}

void enforces_candidate_limits_and_activation_configuration() {
  learning::learning_runner runner({
    .proposer = [](const learning::learning_request&,
                  const learning::learning_context&) {
      return std::vector<learning::learning_candidate> {
        { .proposed_version = "v1" },
        { .proposed_version = "v2" },
        { .proposed_version = "v3" },
      };
    },
    .evaluator = [](const learning::learning_candidate&,
                    const learning::learning_context&) {
      return evaluation_for(0.9);
    },
  });
  const auto bounded = runner.run({ .target = "bounded" }, {
    .policy = { .max_candidates = 2 },
  });
  require(bounded && bounded.proposed_count == 3 && bounded.truncated_count == 1 &&
      bounded.records.size() == 2,
    "learning reports proposals omitted by the evaluation budget");

  bool rejected_configuration = false;
  try {
    (void)runner.run({ .target = "activation" }, {
      .policy = {
        .activation_mode = learning::learning_activation_mode::trusted_automatic,
      },
    });
  }
  catch (const std::invalid_argument&) {
    rejected_configuration = true;
  }
  require(rejected_configuration,
    "activation modes reject a missing activator before proposing changes");
}

void stores_experiences_and_signed_rewards_safely() {
  learning::in_memory_experience_store experiences;
  const auto older = std::chrono::system_clock::now() - 1h;
  const auto newer = std::chrono::system_clock::now();
  const auto first = experiences.add({
    .target = "support.prompt",
    .source = "human_feedback",
    .input = "question one",
    .output = "answer one",
    .created_at = older,
    .metadata = { { "locale", "en" } },
  });
  const auto second = experiences.add({
    .target = "support.prompt",
    .source = "evaluation",
    .input = "question two",
    .output = "answer two",
    .created_at = newer,
    .metadata = { { "locale", "en" } },
  });
  experiences.add({
    .target = "other.prompt",
    .source = "evaluation",
  });

  const auto queried = experiences.query({
    .target = "support.prompt",
    .limit = 1,
    .filters = { { "locale", "en" } },
  });
  require(queried.size() == 1 && queried.front().id == second.id &&
      experiences.get(first.id).has_value(),
    "experience store filters by target and metadata in newest-first order");

  learning::in_memory_reward_store rewards;
  const auto penalty = rewards.add({
    .experience_id = first.id,
    .target = "support.prompt",
    .objective = "correctness",
    .value = -0.75,
    .weight = 2.0,
    .components = { { "factuality", -1.0 }, { "style", 0.25 } },
  });
  rewards.add({
    .experience_id = second.id,
    .target = "support.prompt",
    .objective = "helpfulness",
    .value = 1.0,
  });
  require(rewards.query({ .experience_id = first.id }).front().id == penalty.id,
    "reward ledger accepts signed rewards and queries their provenance");

  for (const auto invalid : {
         std::numeric_limits<double>::quiet_NaN(),
         std::numeric_limits<double>::infinity() }) {
    bool rejected = false;
    try {
      rewards.add({ .target = "support.prompt", .value = invalid });
    }
    catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected, "reward ledger rejects non-finite reward values");
  }
  bool rejected_weight = false;
  try {
    rewards.add({ .target = "support.prompt", .weight = -1.0 });
  }
  catch (const std::invalid_argument&) {
    rejected_weight = true;
  }
  require(rejected_weight, "reward ledger rejects negative reward weights");
}

void registry_tracks_activation_lineage_and_rollback() {
  learning::in_memory_artifact_registry registry;
  registry.stage({
    .target = "support.prompt",
    .version = "v1",
    .kind = learning::learning_change_kind::prompt,
    .artifact = { { "template", "baseline" } },
  });
  const auto first = registry.activate("support.prompt", "v1");
  require(first.active.version == "v1" && !first.previous,
    "first registry activation has no rollback predecessor");
  const auto idempotent = registry.activate("support.prompt", "v1");
  require(idempotent.active.version == "v1" && !idempotent.previous,
    "repeated activation is idempotent and never points rollback at itself");

  registry.stage({
    .target = "support.prompt",
    .version = "v2",
    .parent_version = "v1",
    .kind = learning::learning_change_kind::prompt,
    .artifact = { { "template", "candidate" } },
  });
  const auto promoted = registry.activate("support.prompt", "v2");
  require(promoted.previous && promoted.previous->version == "v1" &&
      registry.get("support.prompt", "v1")->status ==
        learning::artifact_version_status::retired,
    "promotion retires the previous version while preserving lineage");

  const auto rolled_back = registry.rollback("support.prompt", "v1");
  require(rolled_back.active.version == "v1" && rolled_back.previous &&
      rolled_back.previous->version == "v2" &&
      registry.get("support.prompt", "v2")->status ==
        learning::artifact_version_status::rolled_back,
    "rollback activates its explicit target and marks the replaced version");

  bool rejected_duplicate = false;
  try {
    registry.stage({
      .target = "support.prompt",
      .version = "v1",
      .artifact = { { "template", "different" } },
    });
  }
  catch (const std::invalid_argument&) {
    rejected_duplicate = true;
  }
  require(rejected_duplicate,
    "registry rejects reusing a version identifier for different content");
}

void offline_optimizer_receives_scoped_adaptation_data() {
  learning::in_memory_experience_store experiences;
  learning::in_memory_reward_store rewards;
  learning::in_memory_artifact_registry registry;
  registry.stage({
    .target = "support.prompt",
    .version = "v1",
    .artifact = { { "template", "baseline" } },
  });
  registry.activate("support.prompt", "v1");
  const auto relevant = experiences.add({
    .target = "support.prompt",
    .source = "evaluation",
  });
  experiences.add({ .target = "other.prompt" });
  rewards.add({
    .experience_id = relevant.id,
    .target = "support.prompt",
    .value = 1.0,
  });
  rewards.add({ .target = "other.prompt", .value = 1.0 });

  bool received_scoped_data = false;
  auto optimizer = std::make_shared<learning::function_offline_optimizer>(
    [&](const learning::optimization_request& request,
        const learning::learning_context&) {
      received_scoped_data = request.baseline &&
        request.baseline->version == "v1" &&
        request.request.baseline_version == "v1" &&
        request.experiences.size() == 1 &&
        request.rewards.size() == 1 &&
        request.max_candidates == 2;
      return std::vector<learning::learning_candidate> {
        { .proposed_version = "v2" },
        { .proposed_version = "v3" },
        { .proposed_version = "v4" },
      };
    });
  const auto proposer = learning::make_offline_optimizer_proposer(
    optimizer, experiences, &rewards, &registry,
    { .max_candidates = 2 });
  const auto candidates = proposer(
    { .target = "support.prompt" }, learning::learning_context {});
  require(received_scoped_data && candidates.size() == 2 &&
      candidates.front().target == "support.prompt" &&
      candidates.front().parent_version == "v1",
    "offline optimizer receives only target-scoped data and bounded lineage-aware output");
}

void promotes_an_offline_candidate_through_the_complete_gate() {
  learning::in_memory_experience_store experiences;
  learning::in_memory_reward_store rewards;
  learning::in_memory_artifact_registry registry;
  registry.stage({
    .target = "support.prompt",
    .version = "v1",
    .kind = learning::learning_change_kind::prompt,
    .artifact = { { "template", "baseline" } },
  });
  registry.activate("support.prompt", "v1");
  const auto experience = experiences.add({
    .target = "support.prompt",
    .source = "human_feedback",
    .input = "How do I reset my password?",
    .output = "Old answer",
    .expected_output = "Verified reset instructions",
    .feedback_type = learning::feedback_kind::correction,
  });
  rewards.add({
    .experience_id = experience.id,
    .target = "support.prompt",
    .objective = "correctness",
    .value = -1.0,
  });

  auto optimizer = std::make_shared<learning::function_offline_optimizer>(
    [](const learning::optimization_request& request,
       const learning::learning_context&) {
      require(request.baseline && request.experiences.size() == 1 &&
          request.rewards.size() == 1,
        "complete loop supplies baseline, experience, and reward to optimizer");
      return std::vector<learning::learning_candidate> {
        {
          .kind = learning::learning_change_kind::prompt,
          .proposed_version = "v2",
          .artifact = { { "template", "verified reset instructions" } },
          .rationale = "correct the negatively rewarded response",
        },
      };
    });
  approval::allow_all_approval_service approvals;
  learning::learning_runner runner({
    .proposer = learning::make_offline_optimizer_proposer(
      optimizer, experiences, &rewards, &registry),
    .evaluator = [](const learning::learning_candidate&,
                    const learning::learning_context&) {
      return evaluation_for(0.95);
    },
    .activator = learning::make_registry_only_activator(registry),
    .approvals = &approvals,
  });
  const auto result = runner.run({ .target = "support.prompt" }, {
    .policy = {
      .minimum_score_improvement = 0.1,
      .activation_mode = learning::learning_activation_mode::require_approval,
    },
  });
  require(result && result.activated_count == 1 &&
      registry.active("support.prompt")->version == "v2" &&
      result.records.front().activation->previous_version == "v1" &&
      result.records.front().activation->rollback_token == "v1",
    "offline candidate passes evaluation and approval before reversible activation");
}

void activates_one_winner_per_target_and_isolates_telemetry() {
  std::vector<std::string> activated_versions;
  learning::learning_runner runner({
    .proposer = [](const learning::learning_request&,
                  const learning::learning_context&) {
      return std::vector<learning::learning_candidate> {
        {
          .target = "support.prompt",
          .proposed_version = "v2",
          .artifact = { { "score", 0.85 } },
        },
        {
          .target = "support.prompt",
          .proposed_version = "v3",
          .artifact = { { "score", 0.95 } },
        },
        {
          .target = "routing.policy",
          .proposed_version = "r2",
          .artifact = { { "score", 0.9 } },
        },
      };
    },
    .evaluator = [](const learning::learning_candidate& candidate,
                    const learning::learning_context&) {
      return evaluation_for(candidate.artifact.at("score").get<double>());
    },
    .activator = [&](const learning::learning_candidate& candidate,
                     const learning::learning_context&) {
      activated_versions.push_back(candidate.proposed_version);
      return learning::learning_activation_result {
        .activated = true,
        .active_version = candidate.proposed_version,
      };
    },
    .observer = [](const learning::learning_record&) {
      throw std::runtime_error("observer unavailable");
    },
  });

  const auto result = runner.run({ .target = "support.prompt" }, {
    .policy = {
      .activation_mode = learning::learning_activation_mode::trusted_automatic,
    },
  });
  require(result && result.activated_count == 2 &&
      activated_versions == std::vector<std::string>({ "v3", "r2" }),
    "learning activates exactly one highest-ranked candidate per target");
  require(result.records[0].status ==
      learning::learning_candidate_status::not_selected &&
      result.telemetry_error_count == result.records.size(),
    "non-winning accepted candidates are explicit and telemetry cannot undo activation");
}

exploration::exploration_record completed_exploration_record() {
  exploration::exploration_record record;
  record.id = "exploration-1";
  record.request.objective = "reduce response latency";
  record.hypotheses.push_back({
    .value = {
      .id = "hypothesis-1",
      .statement = "Caching reduces latency",
    },
    .experiments = {
      {
        .specification = {
          .id = "experiment-1",
          .hypothesis_id = "hypothesis-1",
          .title = "Read-only benchmark",
        },
        .status = exploration::experiment_status::completed,
        .evidence = exploration::experiment_evidence {
          .succeeded = true,
          .observation = { { "latency_ms", 12 } },
          .summary = "latency improved",
        },
      },
    },
    .assessment = exploration::hypothesis_assessment {
      .verdict = exploration::hypothesis_verdict::supported,
      .confidence = 0.92,
      .conclusion = "benchmark supports caching",
    },
    .verdict = exploration::hypothesis_verdict::supported,
  });
  return record;
}

void imports_exploration_evidence_only_through_the_explicit_adapter() {
  learning::in_memory_experience_store experiences;
  learning::in_memory_reward_store rewards;
  const auto record = completed_exploration_record();
  const auto without_rewards = learning::persist_exploration_experiences(
    record, { .target = "routing.cache-policy" }, experiences, &rewards);
  require(without_rewards.experiences.size() == 1 &&
      without_rewards.rewards.empty() && rewards.query({}).empty(),
    "exploration evidence becomes experience without inventing an implicit reward");
  const auto& imported = without_rewards.experiences.front();
  require(imported.source == "exploration" &&
      imported.source_run_id == record.id &&
      imported.output == "latency improved" &&
      imported.feedback.at("hypothesis_verdict") == "supported" &&
      imported.trajectory.at("id") == "experiment-1",
    "explicit adapter preserves evidence, verdict, trajectory, and provenance");

  const auto with_rewards = learning::persist_exploration_experiences(
    record, { .target = "routing.cache-policy" }, experiences, &rewards,
    [](const exploration::exploration_record&,
       const exploration::hypothesis_record& hypothesis,
       const exploration::experiment_record&,
       const learning::experience_record&) -> std::optional<learning::reward_record> {
      return learning::reward_record {
        .objective = "latency",
        .value = hypothesis.verdict == exploration::hypothesis_verdict::supported
                   ? 1.0 : 0.0,
      };
    });
  require(with_rewards.experiences.size() == 1 &&
      with_rewards.rewards.size() == 1 &&
      with_rewards.rewards.front().experience_id ==
        with_rewards.experiences.front().id &&
      with_rewards.rewards.front().target == "routing.cache-policy",
    "application-supplied mapper creates an attributable reward explicitly");
}

void run(const char* name, void (*test)()) {
  test();
  (void)name;
}

} // namespace

int main() {
  try {
    run("stages candidates", stages_only_candidates_that_pass_promotion_policy);
    run("approval activation", activates_only_after_explicit_approval);
    run("non-standard approval failure",
      nonstandard_approval_failures_are_contained);
    run("suite comparison", evaluation_suite_comparison_detects_regressions);
    run("bounded evaluator", bounds_uncooperative_evaluators);
    run("limits and configuration", enforces_candidate_limits_and_activation_configuration);
    run("experience and reward stores", stores_experiences_and_signed_rewards_safely);
    run("artifact registry", registry_tracks_activation_lineage_and_rollback);
    run("offline optimizer input", offline_optimizer_receives_scoped_adaptation_data);
    run("complete adaptation gate", promotes_an_offline_candidate_through_the_complete_gate);
    run("single winner activation", activates_one_winner_per_target_and_isolates_telemetry);
    run("exploration adapter", imports_exploration_evidence_only_through_the_explicit_adapter);
  }
  catch (const std::exception& ex) {
    std::cerr << "[FAIL] " << ex.what() << '\n';
    return 1;
  }
  return 0;
}
