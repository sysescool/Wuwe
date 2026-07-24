#ifndef WUWE_AGENT_PLANNING_PLAN_PRIORITIZER_HPP
#define WUWE_AGENT_PLANNING_PLAN_PRIORITIZER_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <wuwe/agent/planning/plan.hpp>

namespace wuwe::agent::planning {

using plan_priority_scorer = std::function<double(
  const plan_step&,
  const plan&,
  std::chrono::system_clock::time_point)>;

struct prioritized_plan_step {
  std::size_t index { 0 };
  double score { 0.0 };
  bool deadline_reached { false };
};

class plan_prioritizer {
public:
  explicit plan_prioritizer(
    plan_prioritization_policy policy = {},
    plan_priority_scorer scorer = {})
      : policy_(std::move(policy)), scorer_(std::move(scorer)) {
    validate_policy(policy_);
  }

  [[nodiscard]] prioritized_plan_step evaluate(
    const plan& value,
    std::size_t index,
    std::chrono::system_clock::time_point now =
      std::chrono::system_clock::now()) const {
    if (index >= value.steps.size()) {
      throw std::out_of_range("plan priority index is outside the plan");
    }
    const auto& step = value.steps[index];
    const auto score = scorer_ ? scorer_(step, value, now)
                               : default_score(step, now);
    if (!std::isfinite(score)) {
      throw std::runtime_error("plan priority scorer returned a non-finite score");
    }
    return {
      .index = index,
      .score = score,
      .deadline_reached = step.deadline && *step.deadline <= now,
    };
  }

  [[nodiscard]] std::vector<std::size_t> order(
    const plan& value,
    std::vector<std::size_t> indices,
    std::chrono::system_clock::time_point now =
      std::chrono::system_clock::now()) const {
    if (!policy_.enabled || indices.size() < 2) return indices;

    std::vector<prioritized_plan_step> ranked;
    ranked.reserve(indices.size());
    for (const auto index : indices) ranked.push_back(evaluate(value, index, now));
    std::stable_sort(ranked.begin(), ranked.end(), [&](const auto& left, const auto& right) {
      if (left.score != right.score) return left.score > right.score;
      const auto& lhs = value.steps[left.index];
      const auto& rhs = value.steps[right.index];
      if (lhs.deadline && rhs.deadline && *lhs.deadline != *rhs.deadline) {
        return *lhs.deadline < *rhs.deadline;
      }
      if (lhs.deadline.has_value() != rhs.deadline.has_value()) {
        return lhs.deadline.has_value();
      }
      return left.index < right.index;
    });
    indices.clear();
    indices.reserve(ranked.size());
    for (const auto& item : ranked) indices.push_back(item.index);
    return indices;
  }

private:
  [[nodiscard]] double default_score(
    const plan_step& step,
    std::chrono::system_clock::time_point now) const noexcept {
    double score =
      step.priority * policy_.priority_weight +
      step.urgency * policy_.urgency_weight +
      step.expected_value * policy_.expected_value_weight -
      step.estimated_cost * policy_.estimated_cost_weight;
    if (!step.deadline || policy_.deadline_weight == 0.0) return score;

    if (*step.deadline <= now) {
      return score + policy_.deadline_weight;
    }
    if (policy_.deadline_horizon.count() == 0) return score;
    const auto remaining = std::chrono::duration<double>(*step.deadline - now).count();
    const auto horizon =
      std::chrono::duration<double>(policy_.deadline_horizon).count();
    if (remaining < horizon) {
      score += policy_.deadline_weight * (1.0 - remaining / horizon);
    }
    return score;
  }

  static void validate_policy(const plan_prioritization_policy& policy) {
    const auto finite = [](double value) { return std::isfinite(value); };
    if (!finite(policy.priority_weight) || !finite(policy.urgency_weight) ||
        !finite(policy.expected_value_weight) ||
        !finite(policy.estimated_cost_weight) ||
        !finite(policy.deadline_weight)) {
      throw std::invalid_argument("plan prioritization weights must be finite");
    }
    if (policy.priority_weight < 0.0 || policy.urgency_weight < 0.0 ||
        policy.expected_value_weight < 0.0 ||
        policy.estimated_cost_weight < 0.0 ||
        policy.deadline_weight < 0.0) {
      throw std::invalid_argument(
        "plan prioritization weights must be non-negative");
    }
    if (policy.deadline_horizon.count() < 0) {
      throw std::invalid_argument(
        "plan prioritization deadline horizon must not be negative");
    }
  }

  plan_prioritization_policy policy_;
  plan_priority_scorer scorer_;
};

} // namespace wuwe::agent::planning

#endif // WUWE_AGENT_PLANNING_PLAN_PRIORITIZER_HPP
