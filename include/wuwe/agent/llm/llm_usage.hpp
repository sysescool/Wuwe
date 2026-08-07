#ifndef WUWE_AGENT_LLM_LLM_USAGE_HPP
#define WUWE_AGENT_LLM_LLM_USAGE_HPP

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

#include <wuwe/agent/llm/llm_types.h>

namespace wuwe::agent::llm {

struct llm_pricing {
  double input_per_million_tokens_usd { 0.0 };
  std::optional<double> cached_input_per_million_tokens_usd;
  double output_per_million_tokens_usd { 0.0 };
  std::optional<double> reasoning_per_million_tokens_usd;
};

[[nodiscard]] inline bool valid_llm_pricing(const llm_pricing& pricing) noexcept {
  const auto valid = [](double value) { return std::isfinite(value) && value >= 0.0; };
  return valid(pricing.input_per_million_tokens_usd) &&
         (!pricing.cached_input_per_million_tokens_usd ||
           valid(*pricing.cached_input_per_million_tokens_usd)) &&
         valid(pricing.output_per_million_tokens_usd) &&
         (!pricing.reasoning_per_million_tokens_usd ||
           valid(*pricing.reasoning_per_million_tokens_usd));
}

[[nodiscard]] inline std::optional<llm_cost_breakdown> calculate_llm_cost(
  const llm_usage& usage, const llm_pricing& pricing) noexcept {
  if (!valid_llm_pricing(pricing) || usage.prompt_tokens < 0 || usage.completion_tokens < 0 ||
      usage.total_tokens < 0 || usage.cached_prompt_tokens < 0 || usage.reasoning_tokens < 0 ||
      usage.cached_prompt_tokens > usage.prompt_tokens ||
      usage.reasoning_tokens > usage.completion_tokens) {
    return std::nullopt;
  }

  const auto uncached_input = usage.prompt_tokens - usage.cached_prompt_tokens;
  const auto visible_output = usage.completion_tokens - usage.reasoning_tokens;
  const auto cached_rate =
    pricing.cached_input_per_million_tokens_usd.value_or(pricing.input_per_million_tokens_usd);
  const auto reasoning_rate =
    pricing.reasoning_per_million_tokens_usd.value_or(pricing.output_per_million_tokens_usd);
  constexpr double scale = 1'000'000.0;
  const auto priced = [scale](int tokens, double rate) {
    const auto value = static_cast<double>(tokens) * rate / scale;
    return std::isfinite(value) ? value : (std::numeric_limits<double>::max)();
  };
  llm_cost_breakdown result {
    .input_usd = priced(uncached_input, pricing.input_per_million_tokens_usd),
    .cached_input_usd = priced(usage.cached_prompt_tokens, cached_rate),
    .output_usd = priced(visible_output, pricing.output_per_million_tokens_usd),
    .reasoning_usd = priced(usage.reasoning_tokens, reasoning_rate),
  };
  const auto saturated_add = [](double lhs, double rhs) {
    const auto sum = lhs + rhs;
    return std::isfinite(sum) ? sum : (std::numeric_limits<double>::max)();
  };
  result.total_usd = saturated_add(saturated_add(result.input_usd, result.cached_input_usd),
    saturated_add(result.output_usd, result.reasoning_usd));
  return result;
}

inline void accumulate_llm_usage(llm_usage& target, const llm_usage& value) noexcept {
  const auto add = [](int lhs, int rhs) {
    lhs = (std::max)(lhs, 0);
    if (rhs <= 0)
      return lhs;
    const auto maximum = (std::numeric_limits<int>::max)();
    return rhs > maximum - lhs ? maximum : lhs + rhs;
  };
  target.prompt_tokens = add(target.prompt_tokens, value.prompt_tokens);
  target.completion_tokens = add(target.completion_tokens, value.completion_tokens);
  const auto component_total = add(value.prompt_tokens, value.completion_tokens);
  const auto effective_total = (std::max)(value.total_tokens, component_total);
  target.total_tokens = add(target.total_tokens, effective_total);
  target.total_tokens =
    (std::max)(target.total_tokens, add(target.prompt_tokens, target.completion_tokens));
  target.cached_prompt_tokens = add(target.cached_prompt_tokens, value.cached_prompt_tokens);
  target.reasoning_tokens = add(target.reasoning_tokens, value.reasoning_tokens);
}

} // namespace wuwe::agent::llm

#endif // WUWE_AGENT_LLM_LLM_USAGE_HPP
