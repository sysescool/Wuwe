#ifndef WUWE_AGENT_ROUTING_RESOURCE_AWARE_ROUTER_HPP
#define WUWE_AGENT_ROUTING_RESOURCE_AWARE_ROUTER_HPP

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <wuwe/agent/core/observability.hpp>
#include <wuwe/agent/routing/resource_routing_core.hpp>

namespace wuwe::agent::routing {

using routing_telemetry_failure_mode = observability::telemetry_failure_mode;

struct resource_aware_router_options {
  double quality_weight { 0.5 };
  double latency_weight { 0.2 };
  double cost_weight { 0.3 };
  double preferred_model_bonus { 0.05 };
  model_routing_observer observer;
  observability::event_sink* event_sink {};
  routing_telemetry_failure_mode telemetry_failure_mode { routing_telemetry_failure_mode::ignore };
  double preferred_provider_bonus { 0.05 };
};

class resource_aware_router final : public model_router {
public:
  explicit resource_aware_router(resource_aware_router_options options = {})
      : options_(std::move(options)) {
    if (!std::isfinite(options_.quality_weight) || !std::isfinite(options_.latency_weight) ||
        !std::isfinite(options_.cost_weight) || !std::isfinite(options_.preferred_model_bonus) ||
        !std::isfinite(options_.preferred_provider_bonus) || options_.quality_weight < 0.0 ||
        options_.latency_weight < 0.0 || options_.cost_weight < 0.0 ||
        options_.preferred_model_bonus < 0.0 || options_.preferred_provider_bonus < 0.0) {
      throw std::invalid_argument("resource-aware routing weights must be finite and non-negative");
    }
  }

  resource_aware_router& add(model_resource_profile profile) {
    validate_profile(profile);
    if (find(profile.model, profile.provider)) {
      throw std::invalid_argument(
        "duplicate model resource profile binding: " + profile.provider + "/" + profile.model);
    }
    profiles_.push_back(std::move(profile));
    return *this;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return profiles_.size();
  }

  [[nodiscard]] std::optional<model_resource_profile> find(std::string_view model) const {
    const auto found = std::find_if(
      profiles_.begin(), profiles_.end(), [&](const auto& value) { return value.model == model; });
    return found == profiles_.end() ? std::nullopt : std::optional<model_resource_profile>(*found);
  }

  [[nodiscard]] std::optional<model_resource_profile> find(
    std::string_view model, std::string_view provider) const {
    const auto found = std::find_if(profiles_.begin(), profiles_.end(), [&](const auto& value) {
      return value.model == model && value.provider == provider;
    });
    return found == profiles_.end() ? std::nullopt : std::optional<model_resource_profile>(*found);
  }

  [[nodiscard]] std::vector<model_resource_profile> find_all(std::string_view model) const {
    std::vector<model_resource_profile> output;
    for (const auto& profile : profiles_) {
      if (profile.model == model) {
        output.push_back(profile);
      }
    }
    return output;
  }

  [[nodiscard]] model_route_decision route(const model_route_request& request) const override {
    if (!std::isfinite(request.max_estimated_cost_usd) || request.max_estimated_cost_usd < 0.0) {
      throw std::invalid_argument("routing cost budget must be finite and non-negative");
    }
    if (!std::isfinite(request.requirements.minimum_quality_score) ||
        request.requirements.minimum_quality_score < 0.0 ||
        request.requirements.minimum_quality_score > 1.0) {
      throw std::invalid_argument("minimum model quality must be within [0, 1]");
    }
    model_route_decision decision {
      .estimated_input_tokens = request.estimated_input_tokens,
      .estimated_output_tokens = request.estimated_output_tokens,
      .strategy = request.requirements.strategy,
      .metadata = request.requirements.metadata,
    };
    if (profiles_.empty()) {
      decision.error = model_route_error_code::no_models_registered;
      decision.reason = "no model resource profiles are registered";
      publish(decision);
      return decision;
    }

    bool preferred_model_exists = request.preferred_model.empty();
    bool preferred_provider_exists = request.preferred_provider.empty();
    bool preferred_binding_exists =
      request.preferred_model.empty() || request.preferred_provider.empty();
    std::vector<std::size_t> eligible;
    decision.candidates.reserve(profiles_.size());
    for (const auto& profile : profiles_) {
      model_route_candidate candidate {
        .model = profile.model,
        .provider = profile.provider,
        .estimated_cost_usd = estimate_model_cost_usd(
          profile, request.estimated_input_tokens, request.estimated_output_tokens),
      };
      if (profile.model == request.preferred_model) {
        preferred_model_exists = true;
      }
      if (profile.provider == request.preferred_provider) {
        preferred_provider_exists = true;
      }
      if (profile.model == request.preferred_model &&
          profile.provider == request.preferred_provider) {
        preferred_binding_exists = true;
      }
      evaluate_eligibility(profile, request, candidate);
      candidate.eligible = candidate.rejection_reasons.empty();
      decision.candidates.push_back(std::move(candidate));
      if (decision.candidates.back().eligible) {
        eligible.push_back(decision.candidates.size() - 1);
      }
    }

    if (!preferred_model_exists && !request.requirements.allow_model_override) {
      decision.error = model_route_error_code::preferred_model_unavailable;
      decision.reason = "the required preferred model is not registered";
      publish(decision);
      return decision;
    }
    if (!preferred_provider_exists && !request.requirements.allow_provider_override) {
      decision.error = model_route_error_code::preferred_provider_unavailable;
      decision.reason = "the required preferred provider is not registered";
      publish(decision);
      return decision;
    }
    if (!preferred_binding_exists && !request.requirements.allow_model_override &&
        !request.requirements.allow_provider_override) {
      decision.error = model_route_error_code::no_eligible_model;
      decision.reason = "the required model and provider binding is not registered";
      publish(decision);
      return decision;
    }
    if (eligible.empty()) {
      const auto cost_only_rejection = std::any_of(
        decision.candidates.begin(), decision.candidates.end(), [](const auto& candidate) {
          return !candidate.rejection_reasons.empty() &&
                 std::all_of(candidate.rejection_reasons.begin(),
                   candidate.rejection_reasons.end(),
                   [](const auto& reason) {
                     return reason == "estimated_cost_budget_exceeded" ||
                            reason == "pricing_required";
                   });
        });
      const auto cost_budget_failure = request.max_estimated_cost_usd > 0.0 && cost_only_rejection;
      decision.error = cost_budget_failure ? model_route_error_code::estimated_cost_budget_exceeded
                                           : model_route_error_code::no_eligible_model;
      decision.reason = cost_budget_failure
                          ? "no eligible model fits the remaining estimated cost budget"
                          : "no model satisfies the routing requirements";
      publish(decision);
      return decision;
    }

    score_candidates(request, eligible, decision.candidates);
    const auto best = *std::max_element(eligible.begin(), eligible.end(), [&](auto lhs, auto rhs) {
      const auto& left = decision.candidates[lhs];
      const auto& right = decision.candidates[rhs];
      if (std::abs(left.score - right.score) > 1e-12) {
        return left.score < right.score;
      }
      const bool left_preferred = left.model == request.preferred_model;
      const bool right_preferred = right.model == request.preferred_model;
      if (left_preferred != right_preferred) {
        return !left_preferred;
      }
      const bool left_provider_preferred =
        !request.preferred_provider.empty() && left.provider == request.preferred_provider;
      const bool right_provider_preferred =
        !request.preferred_provider.empty() && right.provider == request.preferred_provider;
      if (left_provider_preferred != right_provider_preferred) {
        return !left_provider_preferred;
      }
      if (left.provider != right.provider) {
        return left.provider > right.provider;
      }
      return left.model > right.model;
    });
    const auto& candidate = decision.candidates[best];
    decision.selected_model = candidate.model;
    decision.selected_provider = candidate.provider;
    decision.estimated_cost_usd = candidate.estimated_cost_usd;
    decision.selected_profile = profiles_[best];
    decision.reason = "selected the highest-scoring eligible model";
    publish(decision);
    return decision;
  }

private:
  static void validate_profile(const model_resource_profile& profile) {
    if (profile.model.empty()) {
      throw std::invalid_argument("model resource profile requires a model id");
    }
    if (!std::isfinite(profile.quality_score) || !std::isfinite(profile.latency_score) ||
        profile.quality_score < 0.0 || profile.quality_score > 1.0 || profile.latency_score < 0.0 ||
        profile.latency_score > 1.0) {
      throw std::invalid_argument(
        "model quality and latency scores must be finite and within [0, 1]");
    }
    const auto valid_price = [](const std::optional<double>& value) {
      return !value || (std::isfinite(*value) && *value >= 0.0);
    };
    if (!valid_price(profile.input_cost_per_million_tokens) ||
        !valid_price(profile.output_cost_per_million_tokens)) {
      throw std::invalid_argument("model token prices must be finite and non-negative");
    }
    if (profile.input_cost_per_million_tokens.has_value() !=
        profile.output_cost_per_million_tokens.has_value()) {
      throw std::invalid_argument("model pricing requires both input and output token prices");
    }
  }

  static void reject(model_route_candidate& candidate, std::string reason) {
    candidate.rejection_reasons.push_back(std::move(reason));
  }

  void evaluate_eligibility(const model_resource_profile& profile,
    const model_route_request& request, model_route_candidate& candidate) const {
    const auto& required = request.requirements;
    if (!profile.available) {
      reject(candidate, "model_unavailable");
    }
    if (!required.allow_model_override && !request.preferred_model.empty() &&
        profile.model != request.preferred_model) {
      reject(candidate, "model_override_disabled");
    }
    if (!required.allow_provider_override && !request.preferred_provider.empty() &&
        profile.provider != request.preferred_provider) {
      reject(candidate, "provider_override_disabled");
    }
    if (profile.context_window_tokens != 0 &&
        (request.estimated_input_tokens > profile.context_window_tokens ||
          request.estimated_output_tokens >
            profile.context_window_tokens - request.estimated_input_tokens)) {
      reject(candidate, "context_window_exceeded");
    }
    if (profile.max_output_tokens != 0 &&
        request.estimated_output_tokens > profile.max_output_tokens) {
      reject(candidate, "max_output_tokens_exceeded");
    }
    if (profile.quality_score < required.minimum_quality_score) {
      reject(candidate, "minimum_quality_not_met");
    }
    if (required.require_tools && !profile.capabilities.tools) {
      reject(candidate, "tools_required");
    }
    if (required.require_parallel_tools && !profile.capabilities.parallel_tools) {
      reject(candidate, "parallel_tools_required");
    }
    if (required.require_streaming && !profile.capabilities.streaming) {
      reject(candidate, "streaming_required");
    }
    if (required.require_reasoning && !profile.capabilities.reasoning) {
      reject(candidate, "reasoning_required");
    }
    if (required.require_json_response && !profile.capabilities.json_response) {
      reject(candidate, "json_response_required");
    }
    if (required.require_json_schema_output && !profile.capabilities.json_schema_output) {
      reject(candidate, "json_schema_output_required");
    }
    if (required.require_stop_sequences && !profile.capabilities.stop_sequences) {
      reject(candidate, "stop_sequences_required");
    }
    if (required.require_deterministic_seed && !profile.capabilities.deterministic_seed) {
      reject(candidate, "deterministic_seed_required");
    }
    if (required.require_explicit_cache_control && !profile.capabilities.explicit_cache_control) {
      reject(candidate, "explicit_cache_control_required");
    }
    if (required.require_local_runtime && !profile.capabilities.local_runtime) {
      reject(candidate, "local_runtime_required");
    }
    const bool pricing_required =
      request.max_estimated_cost_usd > 0.0 ||
      required.strategy == model_selection_strategy::lowest_cost ||
      (required.strategy == model_selection_strategy::balanced && options_.cost_weight > 0.0);
    if (pricing_required && !candidate.estimated_cost_usd && !required.allow_unpriced_models) {
      reject(candidate, "pricing_required");
    }
    if (request.max_estimated_cost_usd > 0.0) {
      if (candidate.estimated_cost_usd &&
          *candidate.estimated_cost_usd > request.max_estimated_cost_usd + 1e-12) {
        reject(candidate, "estimated_cost_budget_exceeded");
      }
    }
  }

  void score_candidates(const model_route_request& request,
    const std::vector<std::size_t>& eligible,
    std::vector<model_route_candidate>& candidates) const {
    double maximum_cost = 0.0;
    for (const auto index : eligible) {
      if (candidates[index].estimated_cost_usd) {
        maximum_cost = (std::max)(maximum_cost, *candidates[index].estimated_cost_usd);
      }
    }

    for (const auto index : eligible) {
      auto& candidate = candidates[index];
      const auto& profile = profiles_[index];
      const auto cost =
        candidate.estimated_cost_usd.value_or(maximum_cost > 0.0 ? maximum_cost : 0.0);
      const auto normalized_cost = maximum_cost > 0.0 ? cost / maximum_cost : 0.0;
      switch (request.requirements.strategy) {
        case model_selection_strategy::lowest_cost:
          candidate.score = 1.0 - normalized_cost;
          break;
        case model_selection_strategy::highest_quality:
          candidate.score = profile.quality_score;
          break;
        case model_selection_strategy::lowest_latency:
          candidate.score = profile.latency_score;
          break;
        case model_selection_strategy::balanced:
          candidate.score = options_.quality_weight * profile.quality_score +
                            options_.latency_weight * profile.latency_score -
                            options_.cost_weight * normalized_cost;
          break;
      }
      if (request.requirements.strategy == model_selection_strategy::balanced &&
          !request.preferred_model.empty() && profile.model == request.preferred_model) {
        candidate.score += options_.preferred_model_bonus;
      }
      if (request.requirements.strategy == model_selection_strategy::balanced &&
          request.requirements.allow_provider_override && !request.preferred_provider.empty() &&
          profile.provider == request.preferred_provider) {
        candidate.score += options_.preferred_provider_bonus;
      }
    }
  }

  void publish(model_route_decision& decision) const {
    std::size_t failures = 0;
    auto invoke = [&](const auto& callback) {
      try {
        callback();
      }
      catch (...) {
        if (options_.telemetry_failure_mode == routing_telemetry_failure_mode::propagate) {
          throw;
        }
        ++failures;
      }
    };
    if (options_.observer) {
      invoke([&] { options_.observer(decision); });
    }
    if (options_.event_sink) {
      const auto trace_id =
        decision.metadata.contains("trace_id") ? decision.metadata.at("trace_id") : std::string {};
      const auto subject_id = decision.metadata.contains("request_id")
                                ? decision.metadata.at("request_id")
                                : std::string {};
      invoke([&] {
        options_.event_sink->publish({
        .module = "resource_routing",
        .name = decision ? "model_selected" : "model_selection_failed",
        .trace_id = trace_id,
        .subject_id = subject_id,
        .attributes = {
          { "error", to_string(decision.error) },
          { "model", decision.selected_model },
          { "provider", decision.selected_provider },
          { "strategy", to_string(decision.strategy) },
          { "candidate_count", std::to_string(decision.candidates.size()) },
        },
      });
      });
    }
    if (failures != 0) {
      decision.metadata["telemetry_error_count"] = std::to_string(failures);
    }
  }

  resource_aware_router_options options_;
  std::vector<model_resource_profile> profiles_;
};

} // namespace wuwe::agent::routing

#endif // WUWE_AGENT_ROUTING_RESOURCE_AWARE_ROUTER_HPP
