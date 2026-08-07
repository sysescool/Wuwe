#include <wuwe/agent/skills/skill_package.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include <wuwe/common/sha256.hpp>

namespace wuwe::agent::skills {
namespace {

bool canonical_sha256(std::string_view value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char item) {
    return (item >= '0' && item <= '9') || (item >= 'a' && item <= 'f');
  });
}

void hash_u64(common::sha256& hash, std::uint64_t value) {
  std::array<std::uint8_t, 8> bytes {};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[bytes.size() - 1 - index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
  hash.update(std::span<const std::uint8_t>(bytes));
}

void hash_field(common::sha256& hash, std::string_view value) {
  hash_u64(hash, static_cast<std::uint64_t>(value.size()));
  hash.update(value);
}

std::string embedded_package_digest(
  const skill_manifest& manifest, const std::map<std::string, skill_resource>& resources) {
  common::sha256 hash;
  constexpr char domain[] = "wuwe.skill.embedded.v1\0";
  hash.update(std::string_view(domain, sizeof(domain) - 1));
  hash_field(hash, skill_manifest_to_json(manifest).dump());
  for (const auto& [id, resource] : resources) {
    hash_field(hash, id);
    hash_field(hash, resource.descriptor.path);
    hash_field(hash, resource.sha256);
    hash_field(hash, resource.content);
  }
  return hash.hex_digest();
}

} // namespace

skill_package::skill_package(skill_manifest manifest, skill_provenance provenance,
  std::map<std::string, skill_resource> resources) {
  validate_skill_manifest(manifest);
  for (const auto& descriptor : manifest.resources) {
    const auto found = resources.find(descriptor.id);
    if (found == resources.end()) {
      throw std::invalid_argument("skill package is missing resource '" + descriptor.id + "'");
    }
    const auto& actual = found->second;
    const auto& actual_descriptor = actual.descriptor;
    if (actual_descriptor.id != descriptor.id || actual_descriptor.path != descriptor.path ||
        actual_descriptor.kind != descriptor.kind ||
        actual_descriptor.media_type != descriptor.media_type ||
        actual_descriptor.size != descriptor.size ||
        actual_descriptor.sha256 != descriptor.sha256 ||
        actual_descriptor.metadata != descriptor.metadata) {
      throw std::invalid_argument(
        "skill package resource descriptor mismatch for '" + descriptor.id + "'");
    }
    if (actual.content.size() != descriptor.size) {
      throw std::invalid_argument(
        "skill package resource size mismatch for '" + descriptor.id + "'");
    }
    if (actual.sha256 != descriptor.sha256 ||
        common::sha256_hex(actual.content) != descriptor.sha256) {
      throw std::invalid_argument(
        "skill package resource digest mismatch for '" + descriptor.id + "'");
    }
  }
  if (resources.size() != manifest.resources.size()) {
    throw std::invalid_argument("skill package contains undeclared resources");
  }
  if (provenance.origin == skill_origin_kind::embedded) {
    provenance.content_sha256 = embedded_package_digest(manifest, resources);
  }
  else if (!canonical_sha256(provenance.content_sha256)) {
    throw std::invalid_argument(
      "non-embedded skill packages require a 64-character lowercase SHA-256 digest");
  }
  data_ = std::make_shared<const data>(
    data { std::move(manifest), std::move(provenance), std::move(resources) });
}

skill_package_create_result skill_package::create(skill_manifest manifest,
  skill_provenance provenance, std::map<std::string, skill_resource> resources) {
  try {
    return {
      .package = std::make_shared<const skill_package>(
        std::move(manifest), std::move(provenance), std::move(resources)),
    };
  }
  catch (const std::invalid_argument& exception) {
    return {
      .error = {
        .code = skill_error_code::invalid_package,
        .message = exception.what(),
      },
    };
  }
}

} // namespace wuwe::agent::skills
