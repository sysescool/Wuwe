#include <wuwe/agent/skills/skill_registry.hpp>

#include <atomic>
#include <stdexcept>

namespace wuwe::agent::skills {

const char* to_string(skill_registration_status value) noexcept {
  switch (value) {
    case skill_registration_status::none:
      return "none";
    case skill_registration_status::inserted:
      return "inserted";
    case skill_registration_status::unchanged:
      return "unchanged";
    case skill_registration_status::replaced:
      return "replaced";
  }
  return "unknown";
}

skill_registry_snapshot::skill_registry_snapshot()
    : packages_(std::make_shared<const package_map>()) {
}

skill_registry_snapshot::skill_registry_snapshot(std::shared_ptr<const package_map> packages)
    : packages_(std::move(packages)) {
}

std::size_t skill_registry_snapshot::size() const noexcept {
  std::size_t output = 0;
  for (const auto& [id, versions] : *packages_) {
    (void)id;
    output += versions.size();
  }
  return output;
}

skill_package_ptr skill_registry_snapshot::find(
  const std::string& id, const semantic_version& version) const noexcept {
  const auto by_id = packages_->find(id);
  if (by_id == packages_->end()) {
    return {};
  }
  const auto by_version = by_id->second.find(version);
  return by_version == by_id->second.end() ? skill_package_ptr {} : by_version->second;
}

skill_package_ptr skill_registry_snapshot::select(
  const std::string& id, const version_requirement& requirement) const noexcept {
  const auto found = packages_->find(id);
  if (found == packages_->end()) {
    return {};
  }
  for (const auto& [version, package] : found->second) {
    if (!requirement.matches(version)) {
      continue;
    }
    return package;
  }
  return {};
}

std::vector<skill_package_ptr> skill_registry_snapshot::versions(const std::string& id) const {
  std::vector<skill_package_ptr> output;
  const auto found = packages_->find(id);
  if (found == packages_->end()) {
    return output;
  }
  output.reserve(found->second.size());
  for (const auto& [version, package] : found->second) {
    (void)version;
    output.push_back(package);
  }
  return output;
}

std::vector<skill_package_ptr> skill_registry_snapshot::packages() const {
  std::vector<skill_package_ptr> output;
  output.reserve(size());
  for (const auto& [id, versions] : *packages_) {
    (void)id;
    for (const auto& [version, package] : versions) {
      (void)version;
      output.push_back(package);
    }
  }
  return output;
}

skill_registry::skill_registry()
    : packages_(std::make_shared<const skill_registry_snapshot::package_map>()) {
}

skill_registration_result skill_registry::register_package(
  skill_package_ptr package, skill_registration_policy policy) {
  if (!package) {
    return {
      .error = {
        .code = skill_error_code::invalid_registration,
        .message = "cannot register a null skill package",
      },
    };
  }
  const auto& descriptor = package->descriptor();
  if (descriptor.id.empty()) {
    return {
      .package = std::move(package),
      .error = {
        .code = skill_error_code::invalid_registration,
        .message = "cannot register a skill package with an empty id",
      },
    };
  }
  const auto id = descriptor.id;
  const auto version = descriptor.version;

  std::lock_guard lock(write_mutex_);
  const auto current = std::atomic_load_explicit(&packages_, std::memory_order_acquire);
  const auto current_id = current->find(id);
  skill_package_ptr existing;
  if (current_id != current->end()) {
    const auto found = current_id->second.find(version);
    if (found != current_id->second.end()) {
      existing = found->second;
    }
  }
  if (existing &&
      (existing == package ||
        existing->provenance().content_sha256 == package->provenance().content_sha256)) {
    return {
      .status = skill_registration_status::unchanged,
      .package = std::move(existing),
    };
  }
  if (existing && policy == skill_registration_policy::reject_conflict) {
    const auto message =
      "skill package '" + id + "@" + version.string() + "' is already registered";
    return {
      .package = std::move(package),
      .previous = existing,
      .error = {
        .code = skill_error_code::registration_conflict,
        .message = message,
      },
    };
  }

  auto next = std::make_shared<skill_registry_snapshot::package_map>(*current);
  auto& versions = (*next)[id];
  versions[version] = std::move(package);
  const auto registered = versions.at(version);
  std::atomic_store_explicit(&packages_,
    std::shared_ptr<const skill_registry_snapshot::package_map>(std::move(next)),
    std::memory_order_release);
  return {
    .status = existing ? skill_registration_status::replaced
                       : skill_registration_status::inserted,
    .package = registered,
    .previous = existing,
  };
}

bool skill_registry::unregister_package(const std::string& id, const semantic_version& version) {
  std::lock_guard lock(write_mutex_);
  const auto current = std::atomic_load_explicit(&packages_, std::memory_order_acquire);
  const auto current_id = current->find(id);
  if (current_id == current->end() || !current_id->second.contains(version)) {
    return false;
  }
  auto next = std::make_shared<skill_registry_snapshot::package_map>(*current);
  auto& versions = next->at(id);
  versions.erase(version);
  if (versions.empty()) {
    next->erase(id);
  }
  std::atomic_store_explicit(&packages_,
    std::shared_ptr<const skill_registry_snapshot::package_map>(std::move(next)),
    std::memory_order_release);
  return true;
}

skill_registry_snapshot skill_registry::snapshot() const noexcept {
  return skill_registry_snapshot(
    std::atomic_load_explicit(&packages_, std::memory_order_acquire));
}

} // namespace wuwe::agent::skills
