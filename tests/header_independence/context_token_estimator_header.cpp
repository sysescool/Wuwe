#include <wuwe/agent/llm/context_token_estimator.hpp>

bool context_token_estimator_header_is_independent() {
  const wuwe::agent::llm::heuristic_context_token_estimator estimator;
  return estimator.estimate_text("independent") != 0;
}
