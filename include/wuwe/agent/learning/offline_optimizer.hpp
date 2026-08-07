#ifndef WUWE_AGENT_LEARNING_OFFLINE_OPTIMIZER_HPP
#define WUWE_AGENT_LEARNING_OFFLINE_OPTIMIZER_HPP

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/learning/adaptation_store.hpp>
#include <wuwe/agent/learning/artifact_registry.hpp>
#include <wuwe/agent/learning/learning_runner.hpp>

namespace wuwe::agent::learning {

struct optimization_request {
  learning_request request;
  std::optional<artifact_version> baseline;
  std::vector<experience_record> experiences;
  std::vector<reward_record> rewards;
  std::size_t max_candidates { 4 };
  std::map<std::string, std::string> metadata;
};

class offline_optimizer {
public:
  virtual ~offline_optimizer() = default;
  [[nodiscard]] virtual std::vector<learning_candidate> optimize(
    const optimization_request& request, const learning_context& context) const = 0;
};

class function_offline_optimizer final : public offline_optimizer {
public:
  using callback = std::function<std::vector<learning_candidate>(
    const optimization_request&, const learning_context&)>;

  explicit function_offline_optimizer(callback value) : callback_(std::move(value)) {
    if (!callback_) {
      throw std::invalid_argument("function_offline_optimizer requires a callback");
    }
  }

  [[nodiscard]] std::vector<learning_candidate> optimize(
    const optimization_request& request, const learning_context& context) const override {
    return callback_(request, context);
  }

private:
  callback callback_;
};

struct optimizer_proposer_options {
  std::size_t max_experiences { 100 };
  std::size_t max_rewards { 100 };
  std::size_t max_candidates { 4 };
  std::map<std::string, std::string> metadata;
};

class offline_optimizer_proposer {
public:
  offline_optimizer_proposer(std::shared_ptr<const offline_optimizer> optimizer,
    experience_store* experiences, reward_store* rewards = nullptr,
    artifact_registry* registry = nullptr, optimizer_proposer_options options = {})
      : optimizer_(std::move(optimizer)), experiences_(experiences), rewards_(rewards),
        registry_(registry), options_(std::move(options)) {
    if (!optimizer_) {
      throw std::invalid_argument("offline optimizer proposer requires an optimizer");
    }
    if (!experiences_) {
      throw std::invalid_argument("offline optimizer proposer requires an experience store");
    }
    if (options_.max_experiences == 0 || options_.max_rewards == 0 ||
        options_.max_candidates == 0) {
      throw std::invalid_argument("offline optimizer limits must be greater than zero");
    }
  }

  [[nodiscard]] std::vector<learning_candidate> operator()(
    const learning_request& request, const learning_context& context) const {
    if (request.target.empty()) {
      throw std::invalid_argument("offline optimizer request target must not be empty");
    }
    if (context.cancellation_requested() || context.deadline_reached())
      return {};
    auto adapted_request = request;
    auto baseline = registry_ ? registry_->active(request.target) : std::nullopt;
    if (adapted_request.baseline_version.empty() && baseline) {
      adapted_request.baseline_version = baseline->version;
    }
    optimization_request optimization {
      .request = adapted_request,
      .baseline = baseline,
      .experiences = experiences_->query({
        .target = request.target,
        .limit = options_.max_experiences,
      }),
      .rewards = rewards_ ? rewards_->query({
                              .target = request.target,
                              .limit = options_.max_rewards,
                            })
                          : std::vector<reward_record> {},
      .max_candidates = options_.max_candidates,
      .metadata = options_.metadata,
    };
    auto candidates = optimizer_->optimize(optimization, context);
    if (candidates.size() > options_.max_candidates) {
      candidates.resize(options_.max_candidates);
    }
    for (auto& candidate : candidates) {
      if (candidate.target.empty())
        candidate.target = request.target;
      if (candidate.parent_version.empty()) {
        candidate.parent_version = adapted_request.baseline_version;
      }
    }
    return candidates;
  }

private:
  std::shared_ptr<const offline_optimizer> optimizer_;
  experience_store* experiences_ {};
  reward_store* rewards_ {};
  artifact_registry* registry_ {};
  optimizer_proposer_options options_;
};

inline learning_proposer make_offline_optimizer_proposer(
  std::shared_ptr<const offline_optimizer> optimizer, experience_store& experiences,
  reward_store* rewards = nullptr, artifact_registry* registry = nullptr,
  optimizer_proposer_options options = {}) {
  auto proposer = std::make_shared<offline_optimizer_proposer>(
    std::move(optimizer), &experiences, rewards, registry, std::move(options));
  return [proposer](const learning_request& request, const learning_context& context) {
    return (*proposer)(request, context);
  };
}

inline learning_activator make_registry_only_activator(artifact_registry& registry) {
  return [&registry](const learning_candidate& candidate, const learning_context& context) {
    if (context.cancellation_requested() || context.deadline_reached()) {
      return learning_activation_result {
        .error = "registry activation cancelled or timed out",
      };
    }
    try {
      registry.stage(artifact_version_from_candidate(candidate));
      auto activation = registry.activate(candidate.target, candidate.proposed_version);
      return learning_activation_result {
        .activated = true,
        .active_version = activation.active.version,
        .previous_version = activation.previous ? activation.previous->version : std::string {},
        .rollback_token = activation.previous ? activation.previous->version : std::string {},
      };
    }
    catch (const std::exception& ex) {
      return learning_activation_result { .error = ex.what() };
    }
    catch (...) {
      return learning_activation_result {
        .error = "registry activation failed with an unknown exception",
      };
    }
  };
}

} // namespace wuwe::agent::learning

#endif // WUWE_AGENT_LEARNING_OFFLINE_OPTIMIZER_HPP
