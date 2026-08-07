#ifndef WUWE_AGENT_LLM_TEXT_TOKEN_ESTIMATOR_HPP
#define WUWE_AGENT_LLM_TEXT_TOKEN_ESTIMATOR_HPP

#include <cstddef>
#include <string>
#include <string_view>

namespace wuwe::agent::llm {

class text_token_estimator {
public:
  virtual ~text_token_estimator() = default;

  [[nodiscard]] virtual std::size_t estimate_text(std::string_view text) const = 0;
  [[nodiscard]] virtual std::string truncate_text(
    std::string_view text, std::size_t token_limit, bool keep_tail) const = 0;
};

} // namespace wuwe::agent::llm

#endif // WUWE_AGENT_LLM_TEXT_TOKEN_ESTIMATOR_HPP
