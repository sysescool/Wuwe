#ifndef WUWE_AGENT_LEARNING_ARTIFACT_REGISTRY_HPP
#define WUWE_AGENT_LEARNING_ARTIFACT_REGISTRY_HPP

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/learning/learning_core.hpp>

namespace wuwe::agent::learning {

enum class artifact_version_status {
  staged,
  active,
  retired,
  rolled_back,
};

[[nodiscard]] inline std::string to_string(artifact_version_status value) {
  switch (value) {
    case artifact_version_status::staged: return "staged";
    case artifact_version_status::active: return "active";
    case artifact_version_status::retired: return "retired";
    case artifact_version_status::rolled_back: return "rolled_back";
  }
  return "unknown";
}

struct artifact_version {
  std::string target;
  std::string version;
  std::string parent_version;
  learning_change_kind kind { learning_change_kind::custom };
  nlohmann::json artifact = nlohmann::json::object();
  std::string rationale;
  artifact_version_status status { artifact_version_status::staged };
  std::chrono::system_clock::time_point created_at {
    std::chrono::system_clock::now()
  };
  std::optional<std::chrono::system_clock::time_point> activated_at;
  std::map<std::string, std::string> metadata;
};

struct artifact_activation {
  artifact_version active;
  std::optional<artifact_version> previous;
};

class artifact_registry {
public:
  virtual ~artifact_registry() = default;
  virtual artifact_version stage(artifact_version value) = 0;
  [[nodiscard]] virtual std::optional<artifact_version> get(
    const std::string& target,
    const std::string& version) const = 0;
  [[nodiscard]] virtual std::optional<artifact_version> active(
    const std::string& target) const = 0;
  [[nodiscard]] virtual std::vector<artifact_version> list(
    const std::string& target) const = 0;
  virtual artifact_activation activate(
    const std::string& target,
    const std::string& version) = 0;
  virtual artifact_activation rollback(
    const std::string& target,
    const std::string& version) = 0;
};

inline artifact_version artifact_version_from_candidate(
  const learning_candidate& candidate) {
  return {
    .target = candidate.target,
    .version = candidate.proposed_version,
    .parent_version = candidate.parent_version,
    .kind = candidate.kind,
    .artifact = candidate.artifact,
    .rationale = candidate.rationale,
    .metadata = candidate.metadata,
  };
}

class in_memory_artifact_registry final : public artifact_registry {
public:
  artifact_version stage(artifact_version value) override {
    validate(value);
    std::scoped_lock lock(mutex_);
    auto& versions = versions_[value.target];
    const auto found = versions.find(value.version);
    if (found != versions.end()) {
      if (found->second.artifact == value.artifact &&
          found->second.parent_version == value.parent_version &&
          found->second.kind == value.kind) {
        return found->second;
      }
      throw std::invalid_argument(
        "artifact version already exists with different content: " + value.version);
    }
    value.status = artifact_version_status::staged;
    versions[value.version] = value;
    return value;
  }

  [[nodiscard]] std::optional<artifact_version> get(
    const std::string& target,
    const std::string& version) const override {
    std::scoped_lock lock(mutex_);
    const auto target_found = versions_.find(target);
    if (target_found == versions_.end()) return std::nullopt;
    const auto version_found = target_found->second.find(version);
    return version_found == target_found->second.end()
             ? std::nullopt
             : std::optional(version_found->second);
  }

  [[nodiscard]] std::optional<artifact_version> active(
    const std::string& target) const override {
    std::scoped_lock lock(mutex_);
    const auto active_found = active_versions_.find(target);
    if (active_found == active_versions_.end()) return std::nullopt;
    return versions_.at(target).at(active_found->second);
  }

  [[nodiscard]] std::vector<artifact_version> list(
    const std::string& target) const override {
    std::scoped_lock lock(mutex_);
    std::vector<artifact_version> output;
    const auto found = versions_.find(target);
    if (found == versions_.end()) return output;
    output.reserve(found->second.size());
    for (const auto& [_, value] : found->second) output.push_back(value);
    return output;
  }

  artifact_activation activate(
    const std::string& target,
    const std::string& version) override {
    std::scoped_lock lock(mutex_);
    return activate_locked(target, version, false);
  }

  artifact_activation rollback(
    const std::string& target,
    const std::string& version) override {
    std::scoped_lock lock(mutex_);
    return activate_locked(target, version, true);
  }

private:
  static void validate(const artifact_version& value) {
    if (value.target.empty()) {
      throw std::invalid_argument("artifact target must not be empty");
    }
    if (value.version.empty()) {
      throw std::invalid_argument("artifact version must not be empty");
    }
    if (value.artifact.is_discarded()) {
      throw std::invalid_argument("artifact payload is invalid");
    }
  }

  artifact_activation activate_locked(
    const std::string& target,
    const std::string& version,
    bool rollback) {
    auto target_found = versions_.find(target);
    if (target_found == versions_.end() ||
        !target_found->second.contains(version)) {
      throw std::out_of_range("artifact version not found: " + target + "/" + version);
    }
    std::optional<artifact_version> previous;
    const auto active_found = active_versions_.find(target);
    if (active_found != active_versions_.end()) {
      auto& current = target_found->second.at(active_found->second);
      if (current.version == version) {
        return { .active = current };
      }
      previous = current;
      current.status = rollback ? artifact_version_status::rolled_back
                                : artifact_version_status::retired;
    }
    auto& selected = target_found->second.at(version);
    selected.status = artifact_version_status::active;
    selected.activated_at = std::chrono::system_clock::now();
    active_versions_[target] = version;
    return { .active = selected, .previous = std::move(previous) };
  }

  mutable std::mutex mutex_;
  std::map<std::string, std::map<std::string, artifact_version>> versions_;
  std::map<std::string, std::string> active_versions_;
};

inline nlohmann::json artifact_version_to_json(const artifact_version& value) {
  return {
    { "target", value.target },
    { "version", value.version },
    { "parent_version", value.parent_version },
    { "kind", to_string(value.kind) },
    { "artifact", value.artifact },
    { "rationale", value.rationale },
    { "status", to_string(value.status) },
    { "metadata", value.metadata },
  };
}

} // namespace wuwe::agent::learning

#endif // WUWE_AGENT_LEARNING_ARTIFACT_REGISTRY_HPP
