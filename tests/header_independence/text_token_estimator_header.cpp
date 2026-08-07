#include <wuwe/agent/llm/text_token_estimator.hpp>

#include <type_traits>

bool text_token_estimator_header_is_independent() {
  return std::has_virtual_destructor_v<wuwe::agent::llm::text_token_estimator>;
}
