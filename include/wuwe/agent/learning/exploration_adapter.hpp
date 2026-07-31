#ifndef WUWE_AGENT_LEARNING_EXPLORATION_ADAPTER_HPP
#define WUWE_AGENT_LEARNING_EXPLORATION_ADAPTER_HPP

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/exploration/exploration_core.hpp>
#include <wuwe/agent/learning/adaptation_store.hpp>

namespace wuwe::agent::learning {

struct exploration_experience_options {
  std::string target;
  bool include_failed { true };
  std::map<std::string, std::string> metadata;
};

using exploration_reward_mapper = std::function<std::optional<reward_record>(
  const exploration::exploration_record&, const exploration::hypothesis_record&,
  const exploration::experiment_record&, const experience_record&)>;

struct exploration_import_result {
  std::vector<experience_record> experiences;
  std::vector<reward_record> rewards;
};

namespace detail {

inline std::optional<experience_record> experience_from_experiment(
  const exploration::exploration_record& record, const exploration::hypothesis_record& hypothesis,
  const exploration::experiment_record& experiment, const exploration_experience_options& options) {
  if (!experiment.evidence)
    return std::nullopt;
  if (!options.include_failed && !experiment.evidence->succeeded) {
    return std::nullopt;
  }

  auto metadata = options.metadata;
  metadata["exploration_id"] = record.id;
  metadata["hypothesis_id"] = hypothesis.value.id;
  metadata["experiment_id"] = experiment.specification.id;
  metadata["experiment_status"] = exploration::to_string(experiment.status);
  metadata["hypothesis_verdict"] = exploration::to_string(hypothesis.verdict);

  nlohmann::json feedback {
    { "experiment_status", exploration::to_string(experiment.status) },
    { "hypothesis_verdict", exploration::to_string(hypothesis.verdict) },
    { "evidence_succeeded", experiment.evidence->succeeded },
    { "evidence_error", experiment.evidence->error },
  };
  if (hypothesis.assessment) {
    feedback["assessment"] = {
      { "confidence", hypothesis.assessment->confidence },
      { "conclusion", hypothesis.assessment->conclusion },
      { "supporting_evidence", hypothesis.assessment->supporting_evidence },
      { "counter_evidence", hypothesis.assessment->counter_evidence },
    };
  }

  auto summary = experiment.evidence->summary;
  if (summary.empty() && !experiment.evidence->error.empty()) {
    summary = experiment.evidence->error;
  }
  return experience_record {
    .id = make_adaptation_id("experience"),
    .target = options.target,
    .source = "exploration",
    .source_run_id = record.id,
    .input = record.request.objective + "\nHypothesis: " + hypothesis.value.statement,
    .output = std::move(summary),
    .feedback_type = feedback_kind::outcome,
    .feedback = std::move(feedback),
    .trajectory = exploration::experiment_record_to_json(experiment),
    .created_at = record.updated_at,
    .metadata = std::move(metadata),
  };
}

} // namespace detail

inline std::vector<experience_record> experiences_from_exploration(
  const exploration::exploration_record& record, exploration_experience_options options) {
  if (options.target.empty()) {
    throw std::invalid_argument("exploration experience target must not be empty");
  }

  std::vector<experience_record> output;
  for (const auto& hypothesis : record.hypotheses) {
    for (const auto& experiment : hypothesis.experiments) {
      auto converted = detail::experience_from_experiment(record, hypothesis, experiment, options);
      if (converted)
        output.push_back(std::move(*converted));
    }
  }
  return output;
}

inline exploration_import_result persist_exploration_experiences(
  const exploration::exploration_record& record, exploration_experience_options options,
  experience_store& experiences, reward_store* rewards = nullptr,
  exploration_reward_mapper reward_mapper = {}) {
  if (reward_mapper && !rewards) {
    throw std::invalid_argument("exploration reward mapper requires a reward store");
  }
  if (options.target.empty()) {
    throw std::invalid_argument("exploration experience target must not be empty");
  }

  exploration_import_result output;
  for (const auto& hypothesis : record.hypotheses) {
    for (const auto& experiment : hypothesis.experiments) {
      auto converted = detail::experience_from_experiment(record, hypothesis, experiment, options);
      if (!converted)
        continue;
      auto mapped_reward = reward_mapper ? reward_mapper(record, hypothesis, experiment, *converted)
                                         : std::optional<reward_record> {};
      if (mapped_reward && !mapped_reward->experience_id.empty() &&
          mapped_reward->experience_id != converted->id) {
        throw std::invalid_argument("exploration reward mapper returned a different experience id");
      }
      if (mapped_reward && !mapped_reward->target.empty() &&
          mapped_reward->target != converted->target) {
        throw std::invalid_argument("exploration reward mapper returned a different target");
      }
      auto persisted = experiences.add(std::move(*converted));
      output.experiences.push_back(persisted);
      if (!mapped_reward)
        continue;
      if (mapped_reward->id.empty()) {
        mapped_reward->id = make_adaptation_id("reward");
      }
      if (mapped_reward->experience_id.empty()) {
        mapped_reward->experience_id = persisted.id;
      }
      if (mapped_reward->target.empty())
        mapped_reward->target = persisted.target;
      if (mapped_reward->source.empty())
        mapped_reward->source = "exploration";
      output.rewards.push_back(rewards->add(std::move(*mapped_reward)));
    }
  }
  return output;
}

} // namespace wuwe::agent::learning

#endif // WUWE_AGENT_LEARNING_EXPLORATION_ADAPTER_HPP
