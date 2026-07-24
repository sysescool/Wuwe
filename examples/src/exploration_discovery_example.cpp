#include <iostream>
#include <vector>

#include <wuwe/agent/exploration/exploration.hpp>
#include <wuwe/agent/learning/exploration_adapter.hpp>

int main() {
  namespace exploration = wuwe::agent::exploration;
  namespace learning = wuwe::agent::learning;

  exploration::in_memory_exploration_store exploration_store;
  exploration::exploration_runner explorer({
    .generate = [](const exploration::exploration_request&,
                   const exploration::exploration_context&) {
      return std::vector<exploration::hypothesis> {
        { .statement = "A smaller retrieval set reduces latency without hurting quality." },
      };
    },
    .design = [](const exploration::exploration_request&,
                 const exploration::hypothesis& hypothesis,
                 const exploration::exploration_context&) {
      return std::vector<exploration::experiment> {
        {
          .hypothesis_id = hypothesis.id,
          .title = "Offline retrieval benchmark",
          .safety = exploration::experiment_safety::read_only,
        },
      };
    },
    .execute = [](const exploration::experiment&,
                  const exploration::exploration_context&) {
      return exploration::experiment_evidence {
        .succeeded = true,
        .observation = { { "latency_reduction", 0.21 }, { "quality_delta", 0.01 } },
        .summary = "latency improved with stable quality",
      };
    },
    .review = [](const exploration::exploration_request&,
                 const exploration::hypothesis&,
                 const std::vector<exploration::experiment_evidence>& evidence,
                 const exploration::exploration_context&) {
      return exploration::hypothesis_assessment {
        .verdict = evidence.empty()
                     ? exploration::hypothesis_verdict::inconclusive
                     : exploration::hypothesis_verdict::supported,
        .confidence = 0.9,
        .conclusion = "offline evidence supports the hypothesis",
      };
    },
    .store = &exploration_store,
  });

  const auto result = explorer.run({
    .objective = "reduce retrieval latency",
  });

  learning::in_memory_experience_store experiences;
  const auto imported = learning::persist_exploration_experiences(
    result.record,
    { .target = "retrieval.configuration" },
    experiences);
  std::cout << "supported=" << result.supported_count
            << ", imported_experiences=" << imported.experiences.size() << '\n';
}
