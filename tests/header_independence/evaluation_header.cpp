#include <wuwe/agent/evaluation/evaluation.hpp>

#ifdef WUWE_AGENT_REASONING_BEST_OF_N_HPP
#error "evaluation.hpp must not force the optional Reasoning adapter"
#endif

bool evaluation_header_is_independent() {
  wuwe::agent::evaluation::evaluation_runner runner;
  return runner.size() == 0;
}
