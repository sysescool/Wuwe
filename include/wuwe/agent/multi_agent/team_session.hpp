#ifndef WUWE_AGENT_MULTI_AGENT_TEAM_SESSION_HPP
#define WUWE_AGENT_MULTI_AGENT_TEAM_SESSION_HPP

#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/multi_agent/multi_agent_core.hpp>

namespace wuwe::agent::multi_agent {

namespace detail {
class active_team_task_guard;
}

struct team_session_snapshot {
  std::string id;
  std::vector<agent_message> messages;
  std::vector<agent_artifact> artifacts;
  std::map<std::string, nlohmann::json> shared_state;
  std::map<std::string, agent_task_status> tasks;
  std::map<std::string, std::string> metadata;
};

class team_session {
public:
  explicit team_session(std::string id, std::map<std::string, std::string> metadata = {})
      : id_(std::move(id)), metadata_(std::move(metadata)) {
    if (id_.empty()) {
      throw std::invalid_argument("team session requires an id");
    }
  }

  [[nodiscard]] const std::string& id() const noexcept {
    return id_;
  }

  void publish(agent_message message) {
    std::scoped_lock lock(mutex_);
    messages_.push_back(std::move(message));
  }

  void publish(agent_artifact artifact) {
    if (artifact.id.empty()) {
      throw std::invalid_argument("team artifact requires an id");
    }
    std::scoped_lock lock(mutex_);
    artifacts_[artifact.id] = std::move(artifact);
  }

  void set(std::string key, nlohmann::json value) {
    if (key.empty()) {
      throw std::invalid_argument("team session state key must not be empty");
    }
    std::scoped_lock lock(mutex_);
    shared_state_[std::move(key)] = std::move(value);
  }

  [[nodiscard]] std::optional<nlohmann::json> get(const std::string& key) const {
    std::scoped_lock lock(mutex_);
    const auto found = shared_state_.find(key);
    return found == shared_state_.end() ? std::nullopt
                                        : std::optional<nlohmann::json>(found->second);
  }

  bool erase(const std::string& key) {
    std::scoped_lock lock(mutex_);
    return shared_state_.erase(key) != 0;
  }

  bool try_start_task(const std::string& task_id) {
    if (task_id.empty()) {
      return false;
    }
    std::scoped_lock lock(mutex_);
    const auto found = tasks_.find(task_id);
    if (found == tasks_.end()) {
      tasks_.emplace(task_id, agent_task_status::submitted);
      return true;
    }
    if (found->second == agent_task_status::submitted ||
        found->second == agent_task_status::working ||
        found->second == agent_task_status::completed ||
        found->second == agent_task_status::timed_out) {
      return false;
    }
    found->second = agent_task_status::submitted;
    return true;
  }

  void update_task(std::string task_id, agent_task_status status) {
    if (task_id.empty()) {
      return;
    }
    std::scoped_lock lock(mutex_);
    tasks_[std::move(task_id)] = status;
  }

  [[nodiscard]] team_session_snapshot snapshot() const {
    std::scoped_lock lock(mutex_);
    team_session_snapshot output {
      .id = id_,
      .messages = messages_,
      .shared_state = shared_state_,
      .tasks = tasks_,
      .metadata = metadata_,
    };
    output.artifacts.reserve(artifacts_.size());
    for (const auto& [_, artifact] : artifacts_) {
      output.artifacts.push_back(artifact);
    }
    return output;
  }

private:
  friend class detail::active_team_task_guard;

  bool fail_task_if_active(const std::string& task_id) {
    std::scoped_lock lock(mutex_);
    const auto found = tasks_.find(task_id);
    if (found == tasks_.end() || (found->second != agent_task_status::submitted &&
                                   found->second != agent_task_status::working)) {
      return false;
    }
    found->second = agent_task_status::failed;
    return true;
  }

  std::string id_;
  mutable std::mutex mutex_;
  std::vector<agent_message> messages_;
  std::map<std::string, agent_artifact> artifacts_;
  std::map<std::string, nlohmann::json> shared_state_;
  std::map<std::string, agent_task_status> tasks_;
  std::map<std::string, std::string> metadata_;
};

} // namespace wuwe::agent::multi_agent

#endif // WUWE_AGENT_MULTI_AGENT_TEAM_SESSION_HPP
