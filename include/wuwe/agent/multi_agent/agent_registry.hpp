#ifndef WUWE_AGENT_MULTI_AGENT_AGENT_REGISTRY_HPP
#define WUWE_AGENT_MULTI_AGENT_AGENT_REGISTRY_HPP

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/multi_agent/multi_agent_core.hpp>

namespace wuwe::agent::multi_agent {

struct registered_agent {
  agent_descriptor descriptor;
  agent_availability availability { agent_availability::available };
  std::size_t active_tasks { 0 };
  agent_executor_capabilities executor_capabilities;
};

namespace detail {

struct agent_registry_entry {
  agent_descriptor descriptor;
  std::shared_ptr<agent_executor> executor;
  agent_availability availability { agent_availability::available };
  std::size_t active_tasks { 0 };
};

struct agent_registry_state {
  mutable std::mutex mutex;
  std::map<std::string, agent_registry_entry> entries;
};

inline bool has_skills(
  const agent_descriptor& descriptor,
  const std::vector<std::string>& required) {
  return std::all_of(required.begin(), required.end(), [&](const auto& id) {
    return std::any_of(descriptor.skills.begin(), descriptor.skills.end(), [&](const auto& skill) {
      return skill.id == id;
    });
  });
}

} // namespace detail

class agent_lease {
public:
  agent_lease() = default;
  agent_lease(const agent_lease&) = delete;
  agent_lease& operator=(const agent_lease&) = delete;

  agent_lease(agent_lease&& other) noexcept
      : state_(std::move(other.state_)),
        descriptor_(std::move(other.descriptor_)),
        executor_(std::move(other.executor_)),
        agent_id_(std::move(other.agent_id_)) {
    other.agent_id_.clear();
  }

  agent_lease& operator=(agent_lease&& other) noexcept {
    if (this != &other) {
      release();
      state_ = std::move(other.state_);
      descriptor_ = std::move(other.descriptor_);
      executor_ = std::move(other.executor_);
      agent_id_ = std::move(other.agent_id_);
      other.agent_id_.clear();
    }
    return *this;
  }

  ~agent_lease() {
    release();
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return executor_ != nullptr;
  }

  [[nodiscard]] const agent_descriptor& descriptor() const noexcept {
    return descriptor_;
  }

  [[nodiscard]] const std::shared_ptr<agent_executor>& executor() const noexcept {
    return executor_;
  }

private:
  friend class agent_registry;

  agent_lease(
    std::shared_ptr<detail::agent_registry_state> state,
    agent_descriptor descriptor,
    std::shared_ptr<agent_executor> executor)
      : state_(std::move(state)),
        descriptor_(std::move(descriptor)),
        executor_(std::move(executor)),
        agent_id_(descriptor_.id) {
  }

  void release() noexcept {
    if (!state_ || agent_id_.empty()) {
      return;
    }
    std::scoped_lock lock(state_->mutex);
    const auto found = state_->entries.find(agent_id_);
    if (found != state_->entries.end() && found->second.active_tasks != 0) {
      --found->second.active_tasks;
    }
    agent_id_.clear();
    executor_.reset();
  }

  std::shared_ptr<detail::agent_registry_state> state_;
  agent_descriptor descriptor_;
  std::shared_ptr<agent_executor> executor_;
  std::string agent_id_;
};

struct agent_acquire_result {
  std::optional<agent_lease> lease;
  agent_task_error_code error { agent_task_error_code::none };
  std::string message;

  [[nodiscard]] explicit operator bool() const noexcept {
    return lease.has_value();
  }
};

class agent_registry {
public:
  agent_registry() : state_(std::make_shared<detail::agent_registry_state>()) {
  }

  agent_registry& add(
    agent_descriptor descriptor,
    std::shared_ptr<agent_executor> executor,
    agent_availability availability = agent_availability::available) {
    validate(descriptor, executor);
    std::scoped_lock lock(state_->mutex);
    if (state_->entries.contains(descriptor.id)) {
      throw std::invalid_argument("duplicate agent id: " + descriptor.id);
    }
    const auto id = descriptor.id;
    state_->entries.emplace(id, detail::agent_registry_entry {
      .descriptor = std::move(descriptor),
      .executor = std::move(executor),
      .availability = availability,
    });
    return *this;
  }

  bool remove(const std::string& id) {
    std::scoped_lock lock(state_->mutex);
    const auto found = state_->entries.find(id);
    if (found == state_->entries.end() || found->second.active_tasks != 0) {
      return false;
    }
    state_->entries.erase(found);
    return true;
  }

  bool set_availability(const std::string& id, agent_availability availability) {
    std::scoped_lock lock(state_->mutex);
    const auto found = state_->entries.find(id);
    if (found == state_->entries.end()) {
      return false;
    }
    found->second.availability = availability;
    return true;
  }

  [[nodiscard]] std::optional<registered_agent> find(const std::string& id) const {
    std::scoped_lock lock(state_->mutex);
    const auto found = state_->entries.find(id);
    if (found == state_->entries.end()) {
      return std::nullopt;
    }
    return snapshot(found->second);
  }

  [[nodiscard]] std::vector<registered_agent> list() const {
    std::scoped_lock lock(state_->mutex);
    std::vector<registered_agent> output;
    output.reserve(state_->entries.size());
    for (const auto& [_, entry] : state_->entries) {
      output.push_back(snapshot(entry));
    }
    return output;
  }

  [[nodiscard]] agent_acquire_result acquire(
    const std::string& preferred_agent,
    const std::vector<std::string>& required_skills) const {
    std::scoped_lock lock(state_->mutex);
    if (!preferred_agent.empty()) {
      const auto found = state_->entries.find(preferred_agent);
      if (found == state_->entries.end()) {
        return { .error = agent_task_error_code::agent_not_found,
          .message = "agent not found: " + preferred_agent };
      }
      return acquire_entry(found->second, required_skills);
    }

    detail::agent_registry_entry* best = nullptr;
    bool capability_match = false;
    bool available_match = false;
    for (auto& [_, entry] : state_->entries) {
      if (!detail::has_skills(entry.descriptor, required_skills)) {
        continue;
      }
      capability_match = true;
      if (entry.availability != agent_availability::available) {
        continue;
      }
      available_match = true;
      if (entry.active_tasks >= entry.descriptor.max_concurrency) {
        continue;
      }
      if (!best || entry.active_tasks < best->active_tasks ||
          (entry.active_tasks == best->active_tasks &&
           entry.descriptor.id < best->descriptor.id)) {
        best = &entry;
      }
    }
    if (!best) {
      if (!capability_match) {
        return { .error = agent_task_error_code::capability_not_found,
          .message = "no registered agent satisfies the required skills" };
      }
      if (!available_match) {
        return { .error = agent_task_error_code::agent_unavailable,
          .message = "all matching agents are draining or offline" };
      }
      return { .error = agent_task_error_code::capacity_exhausted,
        .message = "all matching agents are at capacity" };
    }
    ++best->active_tasks;
    return { .lease = agent_lease(state_, best->descriptor, best->executor) };
  }

private:
  static void validate(
    const agent_descriptor& descriptor,
    const std::shared_ptr<agent_executor>& executor) {
    if (descriptor.id.empty() || descriptor.name.empty()) {
      throw std::invalid_argument("agent descriptor requires id and name");
    }
    if (descriptor.max_concurrency == 0) {
      throw std::invalid_argument("agent max_concurrency must be greater than zero");
    }
    if (!executor) {
      throw std::invalid_argument("agent registration requires an executor");
    }
    if (!executor->capabilities().concurrent_execution &&
        descriptor.max_concurrency > 1) {
      throw std::invalid_argument(
        "non-concurrent agent executors require max_concurrency equal to one");
    }
    std::vector<std::string> skill_ids;
    for (const auto& skill : descriptor.skills) {
      if (skill.id.empty()) {
        throw std::invalid_argument("agent skill requires an id");
      }
      if (std::find(skill_ids.begin(), skill_ids.end(), skill.id) != skill_ids.end()) {
        throw std::invalid_argument("duplicate agent skill id: " + skill.id);
      }
      skill_ids.push_back(skill.id);
    }
  }

  static registered_agent snapshot(const detail::agent_registry_entry& entry) {
    return {
      .descriptor = entry.descriptor,
      .availability = entry.availability,
      .active_tasks = entry.active_tasks,
      .executor_capabilities = entry.executor->capabilities(),
    };
  }

  agent_acquire_result acquire_entry(
    detail::agent_registry_entry& entry,
    const std::vector<std::string>& required_skills) const {
    if (!detail::has_skills(entry.descriptor, required_skills)) {
      return { .error = agent_task_error_code::capability_not_found,
        .message = "preferred agent does not satisfy the required skills" };
    }
    if (entry.availability != agent_availability::available) {
      return { .error = agent_task_error_code::agent_unavailable,
        .message = "preferred agent is not accepting new tasks" };
    }
    if (entry.active_tasks >= entry.descriptor.max_concurrency) {
      return { .error = agent_task_error_code::capacity_exhausted,
        .message = "preferred agent is at capacity" };
    }
    ++entry.active_tasks;
    return { .lease = agent_lease(state_, entry.descriptor, entry.executor) };
  }

  std::shared_ptr<detail::agent_registry_state> state_;
};

} // namespace wuwe::agent::multi_agent

#endif // WUWE_AGENT_MULTI_AGENT_AGENT_REGISTRY_HPP
