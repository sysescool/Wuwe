#include <iostream>
#include <memory>
#include <vector>

#include <wuwe/agent/approval/approval_service.hpp>
#include <wuwe/agent/learning/learning.hpp>

int main() {
  namespace approval = wuwe::agent::approval;
  namespace learning = wuwe::agent::learning;

  learning::in_memory_experience_store experiences;
  learning::in_memory_reward_store rewards;
  learning::in_memory_artifact_registry registry;

  registry.stage({
    .target = "answer.prompt",
    .version = "prompt-v1",
    .kind = learning::learning_change_kind::prompt,
    .artifact = { { "template", "Answer concisely." } },
  });
  registry.activate("answer.prompt", "prompt-v1");

  const auto experience = experiences.add({
    .target = "answer.prompt",
    .source = "evaluation",
    .input = "Explain the result.",
    .output = "An unsupported answer.",
    .expected_output = "A concise answer with evidence.",
    .feedback_type = learning::feedback_kind::correction,
  });
  rewards.add({
    .experience_id = experience.id,
    .target = "answer.prompt",
    .objective = "groundedness",
    .value = -1.0,
  });

  auto optimizer = std::make_shared<learning::function_offline_optimizer>(
    [](const learning::optimization_request& request, const learning::learning_context&) {
      return std::vector<learning::learning_candidate> {
        {
          .kind = learning::learning_change_kind::prompt,
          .proposed_version = "prompt-v2",
          .artifact = {
            { "template", "Answer concisely and cite supporting evidence." },
            { "experience_count", request.experiences.size() },
          },
          .rationale = "address groundedness feedback in the offline ledger",
        },
      };
    });

  approval::allow_all_approval_service approvals;
  learning::learning_runner learner({
    .proposer =
      learning::make_offline_optimizer_proposer(optimizer, experiences, &rewards, &registry),
    .evaluator =
      [](const learning::learning_candidate&, const learning::learning_context&) {
        return learning::learning_evaluation {
          .passed = true,
          .baseline_score = 0.78,
          .candidate_score = 0.9,
          .baseline_pass_rate = 0.9,
          .candidate_pass_rate = 0.98,
        };
      },
    .activator = learning::make_registry_only_activator(registry),
    .approvals = &approvals,
  });

  const auto result = learner.run({
    .goal = "improve grounded answers",
    .target = "answer.prompt",
  }, {
    .policy = {
      .minimum_score_improvement = 0.05,
      .maximum_regressions = 0,
      .activation_mode = learning::learning_activation_mode::require_approval,
    },
  });

  const auto active = registry.active("answer.prompt");
  std::cout << "activated=" << result.activated_count
            << ", active_version=" << (active ? active->version : "none")
            << ", rollback_target=" << result.records.front().activation->rollback_token << '\n';
}
