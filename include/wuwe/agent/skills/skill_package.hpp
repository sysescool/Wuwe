#ifndef WUWE_AGENT_SKILLS_SKILL_PACKAGE_HPP
#define WUWE_AGENT_SKILLS_SKILL_PACKAGE_HPP

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <wuwe/agent/skills/skill_manifest.hpp>

namespace wuwe::agent::skills {

class skill_package final {
public:
  skill_package(skill_manifest manifest, skill_provenance provenance,
    std::map<std::string, skill_resource> resources);

  [[nodiscard]] const skill_manifest& manifest() const noexcept {
    return data_->manifest;
  }
  [[nodiscard]] const skill_descriptor& descriptor() const noexcept {
    return data_->manifest.descriptor;
  }
  [[nodiscard]] const skill_provenance& provenance() const noexcept {
    return data_->provenance;
  }
  [[nodiscard]] const std::map<std::string, skill_resource>& resources() const noexcept {
    return data_->resources;
  }
  [[nodiscard]] const skill_resource* find_resource(const std::string& id) const noexcept {
    const auto found = data_->resources.find(id);
    return found == data_->resources.end() ? nullptr : &found->second;
  }

private:
  struct data {
    skill_manifest manifest;
    skill_provenance provenance;
    std::map<std::string, skill_resource> resources;
  };

  std::shared_ptr<const data> data_;
};

using skill_package_ptr = std::shared_ptr<const skill_package>;

} // namespace wuwe::agent::skills

#endif // WUWE_AGENT_SKILLS_SKILL_PACKAGE_HPP
