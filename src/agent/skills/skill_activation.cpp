#include <wuwe/agent/skills/skill_activation.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <utility>

#include <wuwe/agent/core/execution_observability.hpp>
#include <wuwe/common/sha256.hpp>

namespace wuwe::agent::skills {
namespace {

skill_diagnostic error(std::string code, std::string message, std::string skill_id = {}) {
  return {
    .severity = skill_diagnostic_severity::error,
    .code = std::move(code),
    .message = std::move(message),
    .skill_id = std::move(skill_id),
  };
}

skill_diagnostic warning(std::string code, std::string message, std::string skill_id = {}) {
  return {
    .severity = skill_diagnostic_severity::warning,
    .code = std::move(code),
    .message = std::move(message),
    .skill_id = std::move(skill_id),
  };
}

bool has_errors(const std::vector<skill_diagnostic>& diagnostics) {
  return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
    return diagnostic.severity == skill_diagnostic_severity::error;
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

void merge_capability(std::map<std::string, skill_capability_requirement>& capabilities,
  const skill_capability_requirement& requirement) {
  const auto found = capabilities.find(requirement.name);
  if (found == capabilities.end()) {
    capabilities.emplace(requirement.name, requirement);
    return;
  }
  auto& existing = found->second;
  if (static_cast<int>(requirement.risk) > static_cast<int>(existing.risk)) {
    existing.risk = requirement.risk;
  }
  if (existing.summary.empty()) {
    existing.summary = requirement.summary;
  }
  for (const auto& resource : requirement.resources) {
    if (std::find(existing.resources.begin(), existing.resources.end(), resource) ==
        existing.resources.end()) {
      existing.resources.push_back(resource);
    }
  }
  for (const auto& [key, value] : requirement.metadata) {
    existing.metadata.try_emplace(key, value);
  }
}

std::string activation_fingerprint(const std::vector<skill_package_ptr>& packages) {
  common::sha256 digest;
  constexpr char domain[] = "wuwe.skill.activation.v1\0";
  digest.update(std::string_view(domain, sizeof(domain) - 1));
  auto ordered = packages;
  std::sort(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) {
    const auto& left = lhs->descriptor();
    const auto& right = rhs->descriptor();
    if (left.id != right.id) {
      return left.id < right.id;
    }
    if (left.version.string() != right.version.string()) {
      return left.version.string() < right.version.string();
    }
    return lhs->provenance().content_sha256 < rhs->provenance().content_sha256;
  });
  for (const auto& package : ordered) {
    const auto& descriptor = package->descriptor();
    hash_field(digest, descriptor.id);
    hash_field(digest, descriptor.version.string());
    hash_field(digest, package->provenance().content_sha256);
  }
  return "sha256:" + digest.hex_digest();
}

} // namespace

std::vector<std::string> skill_activation_result::tool_names() const {
  std::vector<std::string> output;
  output.reserve(exposed_tools.size());
  for (const auto& descriptor : exposed_tools) {
    output.push_back(descriptor.name);
  }
  return output;
}

skill_activation_result skill_activator::activate(const skill_activation_request& request) const {
  skill_activation_result result;
  const auto publish = [&](std::string name, std::map<std::string, std::string> attributes = {}) {
    if (!options_.event_sink) {
      return;
    }
    (void)observability::invoke_telemetry(options_.telemetry_failure, [&] {
      options_.event_sink->publish(observability::make_agent_event(
        request.context, "skills", std::move(name), std::move(attributes)));
    });
  };

  publish("skill.activation.started",
    { { "requested_packages", std::to_string(request.resolution.skills.size()) } });

  if (request.context.cancellation_requested()) {
    result.diagnostics.push_back(
      error("activation_cancelled", "skill activation was cancelled before assembly"));
    publish("skill.activation.failed", { { "reason", "activation_cancelled" } });
    return result;
  }
  if (request.context.deadline_reached()) {
    result.diagnostics.push_back(
      error("activation_deadline_exceeded", "skill activation deadline was reached"));
    publish("skill.activation.failed", { { "reason", "activation_deadline_exceeded" } });
    return result;
  }

  if (!request.resolution) {
    result.diagnostics = request.resolution.diagnostics;
    result.diagnostics.push_back(
      error("resolution_failed", "skill activation requires a successful resolution"));
    publish("skill.activation.failed", { { "reason", "resolution_failed" } });
    return result;
  }
  result.diagnostics = request.resolution.diagnostics;
  if (request.resolution.skills.size() > request.limits.max_packages) {
    result.diagnostics.push_back(
      error("package_limit_exceeded", "skill activation exceeds the configured package limit"));
    publish("skill.activation.failed", { { "reason", "package_limit_exceeded" } });
    return result;
  }

  std::map<std::string, tools::tool_descriptor> available_tools;
  for (auto descriptor : request.catalog.tools) {
    try {
      tools::validate_tool_descriptor(descriptor);
    }
    catch (const std::exception& exception) {
      result.diagnostics.push_back(error("invalid_tool_descriptor",
        "runtime catalog tool '" + descriptor.name + "' is invalid: " + exception.what()));
      continue;
    }
    if (!available_tools.emplace(descriptor.name, std::move(descriptor)).second) {
      result.diagnostics.push_back(
        error("duplicate_tool_descriptor", "runtime catalog contains a duplicate tool descriptor"));
    }
  }

  std::set<std::string> exposed_tool_names;
  std::map<std::string, skill_capability_requirement> capabilities;
  std::set<std::string> selected_skill_ids;
  std::size_t instruction_bytes = 0;

  for (const auto& resolved : request.resolution.skills) {
    if (request.context.cancellation_requested()) {
      result.diagnostics.push_back(
        error("activation_cancelled", "skill activation was cancelled during assembly"));
      break;
    }
    if (request.context.deadline_reached()) {
      result.diagnostics.push_back(
        error("activation_deadline_exceeded", "skill activation deadline was reached"));
      break;
    }
    const auto& package = resolved.package;
    if (!package) {
      result.diagnostics.push_back(
        error("null_package", "skill resolution contains a null package"));
      continue;
    }
    const auto& manifest = package->manifest();
    const auto& descriptor = package->descriptor();
    if (!selected_skill_ids.insert(descriptor.id).second) {
      result.diagnostics.push_back(error("duplicate_resolved_skill",
        "skill activation contains more than one package for the same skill id",
        descriptor.id));
      continue;
    }
    result.packages.push_back(package);

    for (const auto& requirement : manifest.tools) {
      const auto found = available_tools.find(requirement.name);
      if (found == available_tools.end()) {
        result.diagnostics.push_back(
          requirement.optional
            ? warning("optional_tool_missing", "optional tool is unavailable", descriptor.id)
            : error("required_tool_missing",
                "required tool is unavailable: " + requirement.name,
                descriptor.id));
        continue;
      }
      if (requirement.exact_version && found->second.version != *requirement.exact_version) {
        result.diagnostics.push_back(
          requirement.optional ? warning("optional_tool_version_mismatch",
                                   "optional tool version does not match: " + requirement.name,
                                   descriptor.id)
                               : error("tool_version_mismatch",
                                   "required tool version does not match: " + requirement.name,
                                   descriptor.id));
        continue;
      }
      exposed_tool_names.insert(requirement.name);
    }

    for (const auto& requirement : manifest.knowledge) {
      if (!request.catalog.knowledge_sources.contains(requirement.source)) {
        result.diagnostics.push_back(
          requirement.optional
            ? warning("optional_knowledge_missing",
                "optional knowledge source is unavailable: " + requirement.source,
                descriptor.id)
            : error("required_knowledge_missing",
                "required knowledge source is unavailable: " + requirement.source,
                descriptor.id));
        continue;
      }
      result.knowledge.push_back(requirement);
    }

    for (const auto& requirement : manifest.capabilities) {
      merge_capability(capabilities, requirement);
    }

    for (const auto& resource_id : manifest.instruction_resources) {
      const auto* resource = package->find_resource(resource_id);
      if (!resource) {
        result.diagnostics.push_back(error("instruction_resource_missing",
          "instruction resource is not present in the immutable package: " + resource_id,
          descriptor.id));
        continue;
      }
      if (resource->descriptor.kind != skill_resource_kind::instructions &&
          resource->descriptor.kind != skill_resource_kind::prompt) {
        result.diagnostics.push_back(error("invalid_instruction_resource_kind",
          "instruction resource must have instructions or prompt kind",
          descriptor.id));
        continue;
      }
      if (resource->content.size() >
          request.limits.max_instruction_bytes -
            (std::min)(instruction_bytes, request.limits.max_instruction_bytes)) {
        result.diagnostics.push_back(error("instruction_budget_exceeded",
          "skill instructions exceed the configured activation budget",
          descriptor.id));
        continue;
      }
      instruction_bytes += resource->content.size();
      result.instructions.push_back({
        .skill_id = descriptor.id,
        .resource_id = resource_id,
        .content = resource->content,
        .provenance = {
          .trust = package->provenance().trust,
          .source = package->provenance().origin == skill_origin_kind::embedded
                      ? core::content_source_kind::application
                      : core::content_source_kind::external,
          .source_id = descriptor.id + "@" + descriptor.version.string(),
          .source_uri = package->provenance().source_uri,
          .metadata = {
            { "package_sha256", package->provenance().content_sha256 },
            { "resource_sha256", resource->sha256 },
          },
        },
      });
    }
  }

  if (exposed_tool_names.size() > request.limits.max_tools) {
    result.diagnostics.push_back(
      error("tool_limit_exceeded", "skill activation exceeds the configured tool limit"));
  }
  if (capabilities.size() > request.limits.max_capabilities) {
    result.diagnostics.push_back(error(
      "capability_limit_exceeded", "skill activation exceeds the capability declaration limit"));
  }
  if (result.knowledge.size() > request.limits.max_knowledge_requirements) {
    result.diagnostics.push_back(error(
      "knowledge_limit_exceeded", "skill activation exceeds the knowledge requirement limit"));
  }

  for (const auto& name : exposed_tool_names) {
    result.exposed_tools.push_back(available_tools.at(name));
  }
  for (auto& [_, requirement] : capabilities) {
    std::sort(requirement.resources.begin(), requirement.resources.end());
    result.declared_capabilities.push_back(std::move(requirement));
  }
  result.fingerprint = activation_fingerprint(result.packages);
  result.success = !has_errors(result.diagnostics);

  publish(result.success ? "skill.activation.completed" : "skill.activation.failed",
    {
      { "packages", std::to_string(result.packages.size()) },
      { "instructions", std::to_string(result.instructions.size()) },
      { "tools", std::to_string(result.exposed_tools.size()) },
      { "diagnostics", std::to_string(result.diagnostics.size()) },
      { "fingerprint", result.fingerprint },
    });
  return result;
}

} // namespace wuwe::agent::skills
