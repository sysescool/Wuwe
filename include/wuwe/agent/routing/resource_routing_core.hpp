#ifndef WUWE_AGENT_ROUTING_RESOURCE_ROUTING_CORE_HPP
#define WUWE_AGENT_ROUTING_RESOURCE_ROUTING_CORE_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/llm/llm_types.h>

namespace wuwe::agent::routing {

enum class model_selection_strategy {
  balanced,
  lowest_cost,
  highest_quality,
  lowest_latency,
};

[[nodiscard]] inline std::string to_string(model_selection_strategy strategy) {
  switch (strategy) {
    case model_selection_strategy::balanced:
      return "balanced";
    case model_selection_strategy::lowest_cost:
      return "lowest_cost";
    case model_selection_strategy::highest_quality:
      return "highest_quality";
    case model_selection_strategy::lowest_latency:
      return "lowest_latency";
  }
  return "unknown";
}

enum class model_route_error_code {
  none,
  no_models_registered,
  preferred_model_unavailable,
  no_eligible_model,
  estimated_cost_budget_exceeded,
  preferred_provider_unavailable,
};

[[nodiscard]] inline std::string to_string(model_route_error_code code) {
  switch (code) {
    case model_route_error_code::none:
      return "none";
    case model_route_error_code::no_models_registered:
      return "no_models_registered";
    case model_route_error_code::preferred_model_unavailable:
      return "preferred_model_unavailable";
    case model_route_error_code::no_eligible_model:
      return "no_eligible_model";
    case model_route_error_code::estimated_cost_budget_exceeded:
      return "estimated_cost_budget_exceeded";
    case model_route_error_code::preferred_provider_unavailable:
      return "preferred_provider_unavailable";
  }
  return "unknown";
}

struct model_capabilities {
  bool tools { false };
  bool parallel_tools { false };
  bool streaming { false };
  bool reasoning { false };
  bool json_response { false };
  bool json_schema_output { false };
  bool stop_sequences { false };
  bool deterministic_seed { false };
  bool explicit_cache_control { false };
  bool local_runtime { false };
};

struct model_resource_profile {
  std::string model;
  std::string provider;
  std::size_t context_window_tokens { 0 };
  std::size_t max_output_tokens { 0 };
  std::optional<double> input_cost_per_million_tokens;
  std::optional<double> output_cost_per_million_tokens;
  double quality_score { 0.5 };
  double latency_score { 0.5 };
  bool available { true };
  model_capabilities capabilities;
  std::map<std::string, std::string> metadata;
};

struct model_route_requirements {
  model_selection_strategy strategy { model_selection_strategy::balanced };
  bool allow_model_override { true };
  bool allow_unpriced_models { false };
  bool require_tools { false };
  bool require_parallel_tools { false };
  bool require_streaming { false };
  bool require_reasoning { false };
  bool require_json_response { false };
  bool require_json_schema_output { false };
  bool require_stop_sequences { false };
  bool require_deterministic_seed { false };
  bool require_explicit_cache_control { false };
  bool require_local_runtime { false };
  double minimum_quality_score { 0.0 };
  std::map<std::string, std::string> metadata;
  bool allow_provider_override { false };
};

struct model_route_request {
  std::string preferred_model;
  std::size_t estimated_input_tokens { 0 };
  std::size_t estimated_output_tokens { 0 };
  double max_estimated_cost_usd { 0.0 };
  model_route_requirements requirements;
  std::string preferred_provider;
};

struct model_route_candidate {
  std::string model;
  std::string provider;
  bool eligible { false };
  std::optional<double> estimated_cost_usd;
  double score { 0.0 };
  std::vector<std::string> rejection_reasons;
};

struct model_route_decision {
  model_route_error_code error { model_route_error_code::none };
  std::string selected_model;
  std::string selected_provider;
  std::optional<model_resource_profile> selected_profile;
  std::optional<double> estimated_cost_usd;
  std::size_t estimated_input_tokens { 0 };
  std::size_t estimated_output_tokens { 0 };
  model_selection_strategy strategy { model_selection_strategy::balanced };
  std::string reason;
  std::vector<model_route_candidate> candidates;
  std::map<std::string, std::string> metadata;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == model_route_error_code::none && !selected_model.empty();
  }
};

using model_routing_observer = std::function<void(const model_route_decision&)>;
using llm_token_estimator = std::function<std::size_t(const llm_request&)>;

class model_router {
public:
  virtual ~model_router() = default;
  [[nodiscard]] virtual model_route_decision route(const model_route_request& request) const = 0;
};

[[nodiscard]] inline std::optional<double> estimate_model_cost_usd(
  const model_resource_profile& profile, std::size_t input_tokens, std::size_t output_tokens) {
  if (!profile.input_cost_per_million_tokens || !profile.output_cost_per_million_tokens) {
    return std::nullopt;
  }
  if (!std::isfinite(*profile.input_cost_per_million_tokens) ||
      !std::isfinite(*profile.output_cost_per_million_tokens) ||
      *profile.input_cost_per_million_tokens < 0.0 ||
      *profile.output_cost_per_million_tokens < 0.0) {
    return std::nullopt;
  }
  const auto input_cost =
    (static_cast<double>(input_tokens) / 1'000'000.0) * *profile.input_cost_per_million_tokens;
  const auto output_cost =
    (static_cast<double>(output_tokens) / 1'000'000.0) * *profile.output_cost_per_million_tokens;
  const auto total = input_cost + output_cost;
  return std::isfinite(total) ? total : (std::numeric_limits<double>::max)();
}

inline void saturating_token_add(std::size_t& target, std::size_t value) noexcept {
  const auto maximum = (std::numeric_limits<std::size_t>::max)();
  target = value > maximum - target ? maximum : target + value;
}

[[nodiscard]] inline std::size_t approximate_text_tokens(std::string_view value) {
  std::size_t ascii_units = 0;
  std::size_t non_ascii_code_points = 0;
  for (std::size_t index = 0; index < value.size();) {
    const auto lead = static_cast<unsigned char>(value[index]);
    if (lead <= 0x7f) {
      ++ascii_units;
      ++index;
      continue;
    }

    std::size_t length = 1;
    if (lead >= 0xc2 && lead <= 0xdf) {
      length = 2;
    }
    else if (lead >= 0xe0 && lead <= 0xef) {
      length = 3;
    }
    else if (lead >= 0xf0 && lead <= 0xf4) {
      length = 4;
    }
    if (index + length > value.size()) {
      length = 1;
    }
    else {
      for (std::size_t offset = 1; offset < length; ++offset) {
        if ((static_cast<unsigned char>(value[index + offset]) & 0xc0) != 0x80) {
          length = 1;
          break;
        }
      }
    }
    ++non_ascii_code_points;
    index += length;
  }
  std::size_t tokens = ascii_units / 4 + (ascii_units % 4 == 0 ? 0U : 1U);
  saturating_token_add(tokens, non_ascii_code_points);
  return tokens;
}

[[nodiscard]] inline std::size_t approximate_request_tokens(const llm_request& request) {
  std::size_t tokens = 2;
  for (const auto& message : request.messages) {
    saturating_token_add(tokens, 4);
    saturating_token_add(tokens, approximate_text_tokens(message.role));
    saturating_token_add(tokens, approximate_text_tokens(message.content));
    if (message.name) {
      saturating_token_add(tokens, approximate_text_tokens(*message.name));
    }
    for (const auto& call : message.tool_calls) {
      saturating_token_add(tokens, approximate_text_tokens(call.name));
      saturating_token_add(tokens, approximate_text_tokens(call.arguments_json));
    }
  }
  for (const auto& tool : request.tools) {
    saturating_token_add(tokens, approximate_text_tokens(tool.name));
    saturating_token_add(tokens, approximate_text_tokens(tool.description));
    saturating_token_add(tokens, approximate_text_tokens(tool.parameters_json_schema));
  }
  return tokens;
}

[[nodiscard]] inline nlohmann::json model_route_candidate_to_json(
  const model_route_candidate& candidate) {
  nlohmann::json output {
    { "model", candidate.model },
    { "provider", candidate.provider },
    { "eligible", candidate.eligible },
    { "score", candidate.score },
    { "rejection_reasons", candidate.rejection_reasons },
  };
  if (candidate.estimated_cost_usd) {
    output["estimated_cost_usd"] = *candidate.estimated_cost_usd;
  }
  return output;
}

[[nodiscard]] inline nlohmann::json model_resource_profile_to_json(
  const model_resource_profile& profile) {
  nlohmann::json output {
    { "model", profile.model },
    { "provider", profile.provider },
    { "context_window_tokens", profile.context_window_tokens },
    { "max_output_tokens", profile.max_output_tokens },
    { "quality_score", profile.quality_score },
    { "latency_score", profile.latency_score },
    { "available", profile.available },
    { "capabilities",
      {
        { "tools", profile.capabilities.tools },
        { "parallel_tools", profile.capabilities.parallel_tools },
        { "streaming", profile.capabilities.streaming },
        { "reasoning", profile.capabilities.reasoning },
        { "json_response", profile.capabilities.json_response },
        { "json_schema_output", profile.capabilities.json_schema_output },
        { "stop_sequences", profile.capabilities.stop_sequences },
        { "deterministic_seed", profile.capabilities.deterministic_seed },
        { "explicit_cache_control", profile.capabilities.explicit_cache_control },
        { "local_runtime", profile.capabilities.local_runtime },
      } },
    { "metadata", profile.metadata },
  };
  if (profile.input_cost_per_million_tokens) {
    output["input_cost_per_million_tokens"] = *profile.input_cost_per_million_tokens;
  }
  if (profile.output_cost_per_million_tokens) {
    output["output_cost_per_million_tokens"] = *profile.output_cost_per_million_tokens;
  }
  return output;
}

[[nodiscard]] inline nlohmann::json model_route_decision_to_json(
  const model_route_decision& decision) {
  auto candidates = nlohmann::json::array();
  for (const auto& candidate : decision.candidates) {
    candidates.push_back(model_route_candidate_to_json(candidate));
  }
  nlohmann::json output {
    { "error", to_string(decision.error) },
    { "selected_model", decision.selected_model },
    { "selected_provider", decision.selected_provider },
    { "estimated_input_tokens", decision.estimated_input_tokens },
    { "estimated_output_tokens", decision.estimated_output_tokens },
    { "strategy", to_string(decision.strategy) },
    { "reason", decision.reason },
    { "candidates", std::move(candidates) },
    { "metadata", decision.metadata },
  };
  if (decision.estimated_cost_usd) {
    output["estimated_cost_usd"] = *decision.estimated_cost_usd;
  }
  if (decision.selected_profile) {
    output["selected_profile"] = model_resource_profile_to_json(*decision.selected_profile);
  }
  return output;
}

} // namespace wuwe::agent::routing

#endif // WUWE_AGENT_ROUTING_RESOURCE_ROUTING_CORE_HPP
