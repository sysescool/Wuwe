#include <limits>
#include <stdexcept>
#include <string>

#include <wuwe/agent/routing/routing.hpp>

using namespace wuwe;
using namespace wuwe::agent;
using namespace wuwe::agent::routing;

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

model_resource_profile profile(
  std::string model,
  double input_price,
  double output_price,
  double quality,
  double latency) {
  return {
    .model = std::move(model),
    .provider = "test",
    .context_window_tokens = 16'000,
    .max_output_tokens = 4'000,
    .input_cost_per_million_tokens = input_price,
    .output_cost_per_million_tokens = output_price,
    .quality_score = quality,
    .latency_score = latency,
    .capabilities = {
      .tools = true,
      .streaming = true,
      .json_response = true,
    },
  };
}

class throwing_event_sink final : public observability::event_sink {
public:
  void publish(const observability::agent_event&) override {
    throw std::runtime_error("event sink unavailable");
  }
};

void strategy_selects_expected_models() {
  resource_aware_router router;
  router.add(profile("economy", 0.10, 0.40, 0.55, 0.95));
  router.add(profile("premium", 2.00, 8.00, 0.95, 0.55));

  const auto cheapest = router.route({
    .estimated_input_tokens = 1'000,
    .estimated_output_tokens = 500,
    .requirements = { .strategy = model_selection_strategy::lowest_cost },
  });
  require(cheapest && cheapest.selected_model == "economy",
    "lowest-cost routing selects the least expensive eligible model");
  const auto serialized = model_route_decision_to_json(cheapest);
  require(serialized.at("selected_profile").at("model") == "economy" &&
      serialized.at("candidates").size() == 2,
    "routing decisions provide a stable structured report");

  const auto highest_quality = router.route({
    .estimated_input_tokens = 1'000,
    .estimated_output_tokens = 500,
    .requirements = { .strategy = model_selection_strategy::highest_quality },
  });
  require(highest_quality && highest_quality.selected_model == "premium",
    "quality routing selects the highest-quality eligible model");

  const auto lowest_latency = router.route({
    .estimated_input_tokens = 1'000,
    .estimated_output_tokens = 500,
    .requirements = { .strategy = model_selection_strategy::lowest_latency },
  });
  require(lowest_latency && lowest_latency.selected_model == "economy",
    "latency routing selects the fastest eligible model");
}

void capabilities_context_and_cost_are_hard_constraints() {
  resource_aware_router router;
  auto economy = profile("economy", 0.10, 0.40, 0.55, 0.95);
  economy.capabilities.reasoning = false;
  auto premium = profile("premium", 2.00, 8.00, 0.95, 0.55);
  premium.capabilities.reasoning = true;
  router.add(economy).add(premium);

  const auto reasoning = router.route({
    .estimated_input_tokens = 500,
    .estimated_output_tokens = 250,
    .requirements = {
      .strategy = model_selection_strategy::lowest_cost,
      .require_reasoning = true,
    },
  });
  require(reasoning && reasoning.selected_model == "premium",
    "capability requirements override a lower model price");

  const auto too_expensive = router.route({
    .estimated_input_tokens = 1'000,
    .estimated_output_tokens = 1'000,
    .max_estimated_cost_usd = 0.0001,
  });
  require(!too_expensive &&
      too_expensive.error == model_route_error_code::estimated_cost_budget_exceeded,
    "routing fails explicitly when no model fits the cost budget");

  const auto too_large = router.route({
    .estimated_input_tokens = 16'000,
    .estimated_output_tokens = 1,
  });
  require(!too_large && too_large.error == model_route_error_code::no_eligible_model,
    "context-window limits are enforced before scoring");
}

void preferred_model_pinning_is_explicit() {
  resource_aware_router router;
  router.add(profile("economy", 0.10, 0.40, 0.55, 0.95));
  router.add(profile("premium", 2.00, 8.00, 0.95, 0.55));

  const auto pinned = router.route({
    .preferred_model = "premium",
    .estimated_input_tokens = 100,
    .estimated_output_tokens = 100,
    .requirements = {
      .strategy = model_selection_strategy::lowest_cost,
      .allow_model_override = false,
    },
  });
  require(pinned && pinned.selected_model == "premium",
    "disabling model override pins a registered preferred model");

  const auto missing = router.route({
    .preferred_model = "missing",
    .estimated_input_tokens = 100,
    .estimated_output_tokens = 100,
    .requirements = { .allow_model_override = false },
  });
  require(!missing && missing.error == model_route_error_code::preferred_model_unavailable,
    "missing pinned models return a stable routing error");
}

void token_estimation_handles_ascii_unicode_and_tools() {
  require(approximate_text_tokens("abcdefgh") == 2,
    "ASCII token estimation uses four-character units");
  require(approximate_text_tokens("你好") == 2,
    "non-ASCII code points are estimated conservatively");

  llm_request request;
  request.messages.push_back({ .role = "user", .content = "hello" });
  const auto without_tools = approximate_request_tokens(request);
  request.tools.push_back({
    .name = "lookup",
    .description = "Find a value",
    .parameters_json_schema = R"({"type":"object"})",
  });
  require(approximate_request_tokens(request) > without_tools,
    "request token estimates include tool schemas");
}

void telemetry_failures_are_isolated() {
  throwing_event_sink event_sink;
  resource_aware_router router({
    .observer = [](const model_route_decision&) {
      throw std::runtime_error("observer unavailable");
    },
    .event_sink = &event_sink,
  });
  router.add(profile("economy", 0.10, 0.40, 0.55, 0.95));

  const auto decision = router.route({
    .estimated_input_tokens = 100,
    .estimated_output_tokens = 100,
  });
  require(decision && decision.metadata.at("telemetry_error_count") == "2",
    "routing telemetry failures do not affect model selection by default");
}

void pricing_requirements_are_explicit() {
  resource_aware_router router;
  model_resource_profile unpriced {
    .model = "unpriced",
    .quality_score = 0.8,
    .latency_score = 0.8,
  };
  router.add(unpriced);

  const auto rejected = router.route({
    .estimated_input_tokens = 100,
    .estimated_output_tokens = 100,
  });
  require(!rejected && rejected.error == model_route_error_code::no_eligible_model,
    "balanced cost-aware routing rejects unknown pricing by default");

  const auto allowed = router.route({
    .estimated_input_tokens = 100,
    .estimated_output_tokens = 100,
    .requirements = { .allow_unpriced_models = true },
  });
  require(allowed && allowed.selected_model == "unpriced",
    "unpriced models require an explicit routing opt-in");

  bool partial_pricing_rejected = false;
  try {
    resource_aware_router invalid;
    auto partial = unpriced;
    partial.model = "partial";
    partial.input_cost_per_million_tokens = 1.0;
    invalid.add(std::move(partial));
  }
  catch (const std::invalid_argument&) {
    partial_pricing_rejected = true;
  }
  require(partial_pricing_rejected,
    "model profiles require input and output pricing as one complete contract");
}

void non_finite_scores_and_weights_are_rejected() {
  bool invalid_weight_rejected = false;
  try {
    resource_aware_router invalid({
      .quality_weight = (std::numeric_limits<double>::quiet_NaN)(),
    });
  }
  catch (const std::invalid_argument&) {
    invalid_weight_rejected = true;
  }
  require(invalid_weight_rejected,
    "routing weights reject non-finite values");

  bool invalid_score_rejected = false;
  try {
    resource_aware_router invalid;
    auto invalid_profile = profile("invalid", 0.1, 0.2, 0.5, 0.5);
    invalid_profile.quality_score = (std::numeric_limits<double>::infinity)();
    invalid.add(std::move(invalid_profile));
  }
  catch (const std::invalid_argument&) {
    invalid_score_rejected = true;
  }
  require(invalid_score_rejected,
    "model profiles reject non-finite quality and latency scores");
}

void extreme_cost_estimates_remain_finite_and_orderable() {
  model_resource_profile extreme {
    .model = "extreme",
    .input_cost_per_million_tokens = (std::numeric_limits<double>::max)(),
    .output_cost_per_million_tokens = (std::numeric_limits<double>::max)(),
    .quality_score = 0.5,
    .latency_score = 0.5,
  };
  const auto estimate = estimate_model_cost_usd(
    extreme,
    (std::numeric_limits<std::size_t>::max)(),
    (std::numeric_limits<std::size_t>::max)());
  require(estimate && std::isfinite(*estimate) &&
      *estimate == (std::numeric_limits<double>::max)(),
    "cost estimation saturates instead of producing infinity");

  resource_aware_router router;
  router.add(std::move(extreme));
  const auto decision = router.route({
    .estimated_input_tokens = (std::numeric_limits<std::size_t>::max)(),
    .estimated_output_tokens = (std::numeric_limits<std::size_t>::max)(),
    .requirements = { .strategy = model_selection_strategy::lowest_cost },
  });
  require(decision && decision.estimated_cost_usd &&
      std::isfinite(*decision.estimated_cost_usd) &&
      std::isfinite(decision.candidates.front().score),
    "extreme finite profiles still produce a deterministic routing decision");
}

void fanout_telemetry_attempts_all_sinks_before_propagating() {
  auto failing = std::make_shared<throwing_event_sink>();
  auto memory = std::make_shared<observability::in_memory_event_sink>();
  observability::fanout_event_sink fanout;
  fanout.add_sink(failing);
  fanout.add_sink(memory);
  bool propagated = false;
  try {
    fanout.publish({ .module = "test", .name = "event" });
  }
  catch (const std::runtime_error&) {
    propagated = true;
  }
  require(propagated && memory->events().size() == 1,
    "fanout telemetry preserves failure while still delivering to later sinks");
}

} // namespace

int main() {
  strategy_selects_expected_models();
  capabilities_context_and_cost_are_hard_constraints();
  preferred_model_pinning_is_explicit();
  token_estimation_handles_ascii_unicode_and_tools();
  telemetry_failures_are_isolated();
  pricing_requirements_are_explicit();
  non_finite_scores_and_weights_are_rejected();
  extreme_cost_estimates_remain_finite_and_orderable();
  fanout_telemetry_attempts_all_sinks_before_propagating();
}
