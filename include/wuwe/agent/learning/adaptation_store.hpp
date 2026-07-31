#ifndef WUWE_AGENT_LEARNING_ADAPTATION_STORE_HPP
#define WUWE_AGENT_LEARNING_ADAPTATION_STORE_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <wuwe/agent/learning/adaptation_core.hpp>

namespace wuwe::agent::learning {

class experience_store {
public:
  virtual ~experience_store() = default;
  virtual experience_record add(experience_record value) = 0;
  [[nodiscard]] virtual std::optional<experience_record> get(const std::string& id) const = 0;
  [[nodiscard]] virtual std::vector<experience_record> query(
    const experience_query& value) const = 0;
  virtual bool update(experience_record value) = 0;
  virtual bool erase(const std::string& id) = 0;
};

class reward_store {
public:
  virtual ~reward_store() = default;
  virtual reward_record add(reward_record value) = 0;
  [[nodiscard]] virtual std::optional<reward_record> get(const std::string& id) const = 0;
  [[nodiscard]] virtual std::vector<reward_record> query(const reward_query& value) const = 0;
  virtual bool erase(const std::string& id) = 0;
};

namespace detail {

inline bool adaptation_filters_match(const std::map<std::string, std::string>& metadata,
  const std::map<std::string, std::string>& filters) {
  for (const auto& [key, value] : filters) {
    const auto found = metadata.find(key);
    if (found == metadata.end() || found->second != value)
      return false;
  }
  return true;
}

template<typename T>
inline void newest_first(std::vector<T>& values) {
  std::stable_sort(values.begin(), values.end(), [](const T& left, const T& right) {
    return left.created_at > right.created_at;
  });
}

} // namespace detail

class in_memory_experience_store final : public experience_store {
public:
  experience_record add(experience_record value) override {
    validate(value);
    if (value.id.empty())
      value.id = make_adaptation_id("experience");
    if (value.created_at.time_since_epoch().count() == 0) {
      value.created_at = std::chrono::system_clock::now();
    }
    std::scoped_lock lock(mutex_);
    if (records_.contains(value.id)) {
      throw std::invalid_argument("experience id already exists: " + value.id);
    }
    records_[value.id] = value;
    return value;
  }

  [[nodiscard]] std::optional<experience_record> get(const std::string& id) const override {
    std::scoped_lock lock(mutex_);
    const auto found = records_.find(id);
    return found == records_.end() ? std::nullopt : std::optional(found->second);
  }

  [[nodiscard]] std::vector<experience_record> query(const experience_query& value) const override {
    std::scoped_lock lock(mutex_);
    std::vector<experience_record> output;
    for (const auto& [_, record] : records_) {
      if (!value.target.empty() && record.target != value.target)
        continue;
      if (!value.source.empty() && record.source != value.source)
        continue;
      if (value.since && record.created_at < *value.since)
        continue;
      if (!detail::adaptation_filters_match(record.metadata, value.filters))
        continue;
      output.push_back(record);
    }
    detail::newest_first(output);
    if (value.limit != 0 && output.size() > value.limit)
      output.resize(value.limit);
    return output;
  }

  bool update(experience_record value) override {
    if (value.id.empty())
      return false;
    validate(value);
    std::scoped_lock lock(mutex_);
    const auto found = records_.find(value.id);
    if (found == records_.end())
      return false;
    found->second = std::move(value);
    return true;
  }

  bool erase(const std::string& id) override {
    std::scoped_lock lock(mutex_);
    return records_.erase(id) != 0;
  }

private:
  static void validate(const experience_record& value) {
    if (value.target.empty()) {
      throw std::invalid_argument("experience target must not be empty");
    }
  }

  mutable std::mutex mutex_;
  std::map<std::string, experience_record> records_;
};

class in_memory_reward_store final : public reward_store {
public:
  reward_record add(reward_record value) override {
    validate(value);
    if (value.id.empty())
      value.id = make_adaptation_id("reward");
    if (value.created_at.time_since_epoch().count() == 0) {
      value.created_at = std::chrono::system_clock::now();
    }
    std::scoped_lock lock(mutex_);
    if (records_.contains(value.id)) {
      throw std::invalid_argument("reward id already exists: " + value.id);
    }
    records_[value.id] = value;
    return value;
  }

  [[nodiscard]] std::optional<reward_record> get(const std::string& id) const override {
    std::scoped_lock lock(mutex_);
    const auto found = records_.find(id);
    return found == records_.end() ? std::nullopt : std::optional(found->second);
  }

  [[nodiscard]] std::vector<reward_record> query(const reward_query& value) const override {
    std::scoped_lock lock(mutex_);
    std::vector<reward_record> output;
    for (const auto& [_, record] : records_) {
      if (!value.target.empty() && record.target != value.target)
        continue;
      if (!value.experience_id.empty() && record.experience_id != value.experience_id)
        continue;
      if (!value.objective.empty() && record.objective != value.objective)
        continue;
      if (value.since && record.created_at < *value.since)
        continue;
      if (!detail::adaptation_filters_match(record.metadata, value.filters))
        continue;
      output.push_back(record);
    }
    detail::newest_first(output);
    if (value.limit != 0 && output.size() > value.limit)
      output.resize(value.limit);
    return output;
  }

  bool erase(const std::string& id) override {
    std::scoped_lock lock(mutex_);
    return records_.erase(id) != 0;
  }

private:
  static void validate(const reward_record& value) {
    if (value.target.empty()) {
      throw std::invalid_argument("reward target must not be empty");
    }
    if (!std::isfinite(value.value) || !std::isfinite(value.weight) || value.weight < 0.0) {
      throw std::invalid_argument(
        "reward value must be finite and reward weight must be finite and non-negative");
    }
    for (const auto& [_, component] : value.components) {
      if (!std::isfinite(component)) {
        throw std::invalid_argument("reward components must be finite");
      }
    }
  }

  mutable std::mutex mutex_;
  std::map<std::string, reward_record> records_;
};

} // namespace wuwe::agent::learning

#endif // WUWE_AGENT_LEARNING_ADAPTATION_STORE_HPP
