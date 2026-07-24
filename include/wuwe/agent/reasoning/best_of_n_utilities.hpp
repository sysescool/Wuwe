#ifndef WUWE_AGENT_REASONING_BEST_OF_N_UTILITIES_HPP
#define WUWE_AGENT_REASONING_BEST_OF_N_UTILITIES_HPP

#include <wuwe/agent/reasoning/best_of_n.hpp>

namespace wuwe::agent::reasoning {

inline best_of_n_candidate_generator make_reasoning_candidate_generator(
  reasoning_runner runner) {
  return [runner = std::move(runner)](
           const reasoning_request& request,
           const best_of_n_context& context) {
    const auto isolated =
      context.side_effects == best_of_n_side_effect_policy::isolate;
    return runner.run(request, reasoning_run_options {
      .stop_token = context.stop_token,
      .effects = {
        .allow_tool_calls = !isolated,
        .allow_plan_execution = !isolated,
        .persist_memory = !isolated,
        .persist_plan = !isolated,
        .persist_reflection = !isolated,
      },
    });
  };
}

inline bool commit_best_of_n_result(
  const reasoning_runner& runner,
  const best_of_n_result& result) {
  const auto* selected = result.selected_candidate();
  if (!selected) return false;
  runner.commit_selected_result(selected->result);
  return true;
}

inline best_of_n_selector make_majority_vote_selector(best_of_n_vote_key vote_key) {
  if (!vote_key) {
    throw std::invalid_argument("majority vote selector requires a vote key function");
  }
  return [vote_key = std::move(vote_key)](
           const std::vector<best_of_n_candidate>& candidates)
           -> std::optional<std::size_t> {
    struct vote_group {
      std::size_t count { 0 };
      double total_score { 0.0 };
      const best_of_n_candidate* representative {};
    };
    std::map<std::string, vote_group> groups;
    for (const auto& candidate : candidates) {
      if (!candidate.eligible()) continue;
      auto& group = groups[vote_key(candidate)];
      ++group.count;
      group.total_score += candidate.score->value;
      if (!group.representative ||
          candidate.score->value > group.representative->score->value ||
          (candidate.score->value == group.representative->score->value &&
           candidate.index < group.representative->index)) {
        group.representative = &candidate;
      }
    }

    const vote_group* best = nullptr;
    for (const auto& [key, group] : groups) {
      (void)key;
      if (!best || group.count > best->count ||
          (group.count == best->count && group.total_score > best->total_score) ||
          (group.count == best->count && group.total_score == best->total_score &&
           group.representative->index < best->representative->index)) {
        best = &group;
      }
    }
    return best ? std::optional(best->representative->index) : std::nullopt;
  };
}

inline nlohmann::json best_of_n_score_to_json(const best_of_n_score& score) {
  return {
    { "value", score.value },
    { "accepted", score.accepted },
    { "rationale", score.rationale },
    { "metadata", score.metadata },
    { "usage", reasoning_usage_to_json(score.usage) },
  };
}

inline nlohmann::json best_of_n_event_to_json(const best_of_n_event& event) {
  return {
    { "sequence", event.sequence },
    { "type", to_string(event.type) },
    { "candidate_index", event.candidate_index
                             ? nlohmann::json(*event.candidate_index)
                             : nlohmann::json(nullptr) },
    { "score", event.score ? nlohmann::json(*event.score) : nlohmann::json(nullptr) },
    { "message", event.message },
    { "elapsed_ms", event.elapsed.count() },
    { "metadata", event.metadata },
  };
}

inline nlohmann::json best_of_n_result_to_json(const best_of_n_result& result) {
  auto candidates = nlohmann::json::array();
  for (const auto& candidate : result.candidates) {
    candidates.push_back({
      { "index", candidate.index },
      { "status", to_string(candidate.status) },
      { "score", candidate.score
                     ? best_of_n_score_to_json(*candidate.score)
                     : nlohmann::json(nullptr) },
      { "result", reasoning_result_to_json(candidate.result) },
      { "error", candidate.error },
      { "elapsed_ms", candidate.elapsed.count() },
      { "detached", candidate.detached },
    });
  }
  auto trace = nlohmann::json::array();
  for (const auto& event : result.trace) trace.push_back(best_of_n_event_to_json(event));
  return {
    { "completed", result.completed },
    { "selected_index", result.selected_index
                            ? nlohmann::json(*result.selected_index)
                            : nlohmann::json(nullptr) },
    { "stop_reason", to_string(result.stop_reason) },
    { "error", result.error },
    { "elapsed_ms", result.elapsed.count() },
    { "eligible_count", result.eligible_count },
    { "rejected_count", result.rejected_count },
    { "failed_count", result.failed_count },
    { "budget_exceeded_count", result.budget_exceeded_count },
    { "side_effect_blocked_count", result.side_effect_blocked_count },
    { "cancelled_count", result.cancelled_count },
    { "timed_out_count", result.timed_out_count },
    { "skipped_count", result.skipped_count },
    { "detached_count", result.detached_count },
    { "coordination_detached_count", result.coordination_detached_count },
    { "telemetry_error_count", result.telemetry_error_count },
    { "aggregate_usage", reasoning_usage_to_json(result.aggregate_usage) },
    { "budget_accounted_usage", reasoning_usage_to_json(result.budget_accounted_usage) },
    { "outstanding_reserved_usage",
      reasoning_usage_to_json(result.outstanding_reserved_usage) },
    { "candidates", std::move(candidates) },
    { "trace", std::move(trace) },
  };
}

} // namespace wuwe::agent::reasoning

#endif // WUWE_AGENT_REASONING_BEST_OF_N_UTILITIES_HPP
