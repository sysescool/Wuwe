#ifndef WUWE_AGENT_EVALUATION_REASONING_EVALUATION_HPP
#define WUWE_AGENT_EVALUATION_REASONING_EVALUATION_HPP

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <wuwe/agent/evaluation/evaluation_core.hpp>
#include <wuwe/agent/reasoning/best_of_n.hpp>

namespace wuwe::agent::evaluation {

inline evaluation_case evaluation_case_from_reasoning(
  std::string id,
  std::string input,
  const reasoning::reasoning_result& result,
  std::string expected_output = {},
  std::map<std::string, std::string> metadata = {}) {
  return {
    .id = std::move(id),
    .input = std::move(input),
    .output = result.content,
    .expected_output = std::move(expected_output),
    .actual = reasoning::reasoning_result_to_json(result),
    .trajectory = reasoning::reasoning_trace_to_json(result.trace),
    .elapsed = result.elapsed,
    .metadata = std::move(metadata),
  };
}

inline evaluation_case evaluation_case_from_best_of_n(
  std::string id,
  std::string input,
  const reasoning::best_of_n_result& result,
  std::string expected_output = {},
  std::map<std::string, std::string> metadata = {}) {
  const auto* selected = result.selected_candidate();
  return {
    .id = std::move(id),
    .input = std::move(input),
    .output = selected ? selected->result.content : std::string {},
    .expected_output = std::move(expected_output),
    .actual = reasoning::best_of_n_result_to_json(result),
    .trajectory = [&result] {
      auto trace = nlohmann::json::array();
      for (const auto& event : result.trace) {
        trace.push_back(reasoning::best_of_n_event_to_json(event));
      }
      return trace;
    }(),
    .elapsed = result.elapsed,
    .metadata = std::move(metadata),
  };
}

inline reasoning::best_of_n_candidate_scorer make_candidate_evaluator_scorer(
  std::shared_ptr<const evaluator> value,
  std::string expected_output = {},
  bool require_pass = true) {
  if (!value) {
    throw std::invalid_argument("candidate evaluator scorer requires an evaluator");
  }
  return [value = std::move(value),
          expected_output = std::move(expected_output),
          require_pass](
           const reasoning::reasoning_request& request,
           const reasoning::reasoning_result& result,
           const reasoning::best_of_n_context& context) {
    auto evaluation = evaluation_case_from_reasoning(
      "candidate-" + std::to_string(context.index),
      request.input,
      result,
      expected_output,
      request.metadata);
    auto metric = value->evaluate(evaluation);
    return reasoning::best_of_n_score {
      .value = metric.score,
      .accepted = !require_pass || metric.passed,
      .rationale = std::move(metric.explanation),
      .metadata = std::move(metric.metadata),
    };
  };
}

} // namespace wuwe::agent::evaluation

#endif // WUWE_AGENT_EVALUATION_REASONING_EVALUATION_HPP
