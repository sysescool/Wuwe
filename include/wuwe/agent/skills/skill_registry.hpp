#ifndef WUWE_AGENT_SKILLS_SKILL_REGISTRY_HPP
#define WUWE_AGENT_SKILLS_SKILL_REGISTRY_HPP

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <wuwe/agent/skills/skill_package.hpp>

namespace wuwe::agent::skills {

enum class skill_registration_policy {
  reject_conflict,
  replace,
};

class skill_registry_snapshot final {
public:
  using version_map = std::map<semantic_version, skill_package_ptr, std::greater<>>;
  using package_map = std::map<std::string, version_map>;

  skill_registry_snapshot();

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept {
    return size() == 0;
  }
  [[nodiscard]] skill_package_ptr find(
    const std::string& id, const semantic_version& version) const noexcept;
  [[nodiscard]] skill_package_ptr select(
    const std::string& id, const version_requirement& requirement) const noexcept;
  [[nodiscard]] std::vector<skill_package_ptr> versions(const std::string& id) const;
  [[nodiscard]] std::vector<skill_package_ptr> packages() const;

private:
  friend class skill_registry;
  explicit skill_registry_snapshot(std::shared_ptr<const package_map> packages);
  std::shared_ptr<const package_map> packages_;
};

class skill_registry final {
public:
  skill_registry();

  void register_package(skill_package_ptr package,
    skill_registration_policy policy = skill_registration_policy::reject_conflict);
  [[nodiscard]] bool unregister_package(const std::string& id, const semantic_version& version);
  [[nodiscard]] skill_registry_snapshot snapshot() const noexcept;

private:
  mutable std::mutex write_mutex_;
  std::atomic<std::shared_ptr<const skill_registry_snapshot::package_map>> packages_;
};

} // namespace wuwe::agent::skills

#endif // WUWE_AGENT_SKILLS_SKILL_REGISTRY_HPP
