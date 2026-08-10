#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <wuwe/agent/skills/skill_manifest.hpp>
#include <wuwe/agent/skills/skill_package.hpp>
#include <wuwe/agent/skills/skill_registry.hpp>
#include <wuwe/agent/skills/skill_resolver.hpp>
#include <wuwe/agent/skills/skill_version.hpp>

using namespace wuwe::agent::skills;

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template<typename Function>
void require_invalid(Function function, const char* message) {
  try {
    function();
  }
  catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error(message);
}

skill_package_ptr package(
  std::string id, std::string version, std::vector<skill_dependency> dependencies = {}) {
  skill_manifest manifest;
  manifest.descriptor = {
    .id = std::move(id),
    .name = "Test skill",
    .description = "A test skill package",
    .version = semantic_version::parse(version),
  };
  manifest.dependencies = std::move(dependencies);
  return std::make_shared<const skill_package>(
    std::move(manifest), skill_provenance {}, std::map<std::string, skill_resource> {});
}

skill_dependency dependency(std::string id, std::string requirement, bool optional = false) {
  return {
    .id = std::move(id),
    .version = version_requirement::parse(requirement),
    .optional = optional,
  };
}

skill_registration_result require_registered(skill_registry& registry, skill_package_ptr value,
  skill_registration_policy policy = skill_registration_policy::reject_conflict) {
  auto result = registry.register_package(std::move(value), policy);
  require(static_cast<bool>(result), "skill package registration succeeds");
  return result;
}

void semver_is_strict_and_follows_precedence_rules() {
  const std::vector<std::string> ordered {
    "1.0.0-alpha",
    "1.0.0-alpha.1",
    "1.0.0-alpha.beta",
    "1.0.0-beta",
    "1.0.0-beta.2",
    "1.0.0-beta.11",
    "1.0.0-rc.1",
    "1.0.0",
  };
  for (std::size_t index = 1; index < ordered.size(); ++index) {
    require(semantic_version::parse(ordered[index - 1]) < semantic_version::parse(ordered[index]),
      "SemVer prerelease precedence follows the normative ordering");
  }
  const auto linux_version = semantic_version::parse("1.2.3+linux");
  const auto windows_version = semantic_version::parse("1.2.3+windows");
  require(linux_version != windows_version,
    "SemVer build metadata participates in complete version identity");
  require(compare_precedence(linux_version, windows_version) == std::strong_ordering::equal,
    "SemVer build metadata does not affect precedence");
  require(semantic_version::parse("1.2.3-alpha.9").string() == "1.2.3-alpha.9",
    "SemVer round-trips through its canonical representation");

  for (const auto* invalid : { "1",
         "1.2",
         "01.2.3",
         "1.02.3",
         "1.2.03",
         "1.2.3-01",
         "1.2.3-",
         "1.2.3+",
         " 1.2.3",
         "1.2.3+bad!" }) {
    require_invalid(
      [&] { (void)semantic_version::parse(invalid); }, "invalid SemVer input is rejected");
  }
}

void version_requirements_cover_exact_ranges_caret_and_tilde() {
  require(version_requirement::parse("1.2.3").matches(semantic_version::parse("1.2.3")),
    "bare versions are exact requirements");
  require(!version_requirement::parse("1.2.3").matches(semantic_version::parse("1.2.4")),
    "exact requirements exclude later patches");
  require(
    !version_requirement::parse("1.2.3+linux").matches(semantic_version::parse("1.2.3+windows")),
    "exact requirements include build metadata even though precedence ignores it");
  require(version_requirement::parse(">=1.2.0 <2.0.0").matches(semantic_version::parse("1.9.9")),
    "comparison ranges accept versions inside all bounds");
  require(!version_requirement::parse(">=1.2.0 <2.0.0").matches(semantic_version::parse("2.0.0")),
    "comparison ranges enforce exclusive bounds");
  require(version_requirement::parse("^1.2.3").matches(semantic_version::parse("1.9.0")) &&
            !version_requirement::parse("^1.2.3").matches(semantic_version::parse("2.0.0")),
    "caret ranges preserve the left-most non-zero component");
  require(version_requirement::parse("^0.2.3").matches(semantic_version::parse("0.2.9")) &&
            !version_requirement::parse("^0.2.3").matches(semantic_version::parse("0.3.0")),
    "caret ranges handle zero-major versions");
  require(version_requirement::parse("~1.2.3").matches(semantic_version::parse("1.2.99")) &&
            !version_requirement::parse("~1.2.3").matches(semantic_version::parse("1.3.0")),
    "tilde ranges remain within the minor release");
  require(!version_requirement::parse("*").matches(semantic_version::parse("2.0.0-beta.1")) &&
            version_requirement::parse(">=2.0.0-beta.1 <2.0.0")
              .matches(semantic_version::parse("2.0.0-beta.2")),
    "prereleases require an explicit prerelease comparator");
  require_invalid([&] { (void)version_requirement::parse(">=1.0.0,"); },
    "version ranges reject trailing empty comparators");
  require_invalid([&] { (void)version_requirement::parse(">=1.0.0, ,<2.0.0"); },
    "version ranges reject repeated empty comparators");
}

void manifest_parser_is_strict_and_bounded() {
  const std::string valid = R"({
    "schema_version": 1,
    "id": "com.example.review",
    "version": "1.2.0",
    "name": "Code review",
    "description": "Reviews a change set",
    "tags": ["review"],
    "input_schema": {"type":"object"},
    "output_schema": {"type":"object"},
    "instructions": ["main"],
    "resources": [{
      "id":"main", "path":"prompts/main.md", "kind":"instructions",
      "media_type":"text/markdown", "size":12,
      "sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    }],
    "requires": {
      "skills": [{"id":"com.example.base", "version":"^1.0.0", "optional":false}],
      "tools": [{"name":"filesystem.read", "optional":false}],
      "capabilities": [{"name":"filesystem.read", "risk":"low", "summary":"Read code"}],
      "knowledge": [{"source":"coding-policy", "max_context_chars":4096, "optional":true}]
    },
    "metadata": {"owner":"platform"}
  })";
  const auto parsed = parse_skill_manifest(valid);
  require(parsed.descriptor.id == "com.example.review" && parsed.resources.size() == 1 &&
            parsed.dependencies.size() == 1 && parsed.capabilities.size() == 1,
    "strict manifest parsing retains all declared contract sections");
  const auto parsed_result = try_parse_skill_manifest(valid);
  require(parsed_result && parsed_result.manifest->descriptor.id == "com.example.review",
    "manifest parsing provides a result-based API for external data");
  require(skill_manifest_from_json(skill_manifest_to_json(parsed)).descriptor.version ==
            parsed.descriptor.version,
    "manifest JSON serialization is round-trip safe");

  auto unknown = nlohmann::json::parse(valid);
  unknown["surprise"] = true;
  require_invalid([&] { (void)skill_manifest_from_json(unknown); },
    "unknown top-level manifest fields are rejected");
  auto nested_unknown = nlohmann::json::parse(valid);
  nested_unknown["requires"]["skills"][0]["surprise"] = true;
  require_invalid([&] { (void)skill_manifest_from_json(nested_unknown); },
    "unknown nested manifest fields are rejected");
  auto traversal = nlohmann::json::parse(valid);
  traversal["resources"][0]["path"] = "../secret";
  require_invalid([&] { (void)skill_manifest_from_json(traversal); },
    "resource paths cannot escape a skill package");
  for (const auto* invalid_path : { "data:stream", "folder/trailing.", "CON.txt" }) {
    auto nonportable = nlohmann::json::parse(valid);
    nonportable["resources"][0]["path"] = invalid_path;
    require_invalid([&] { (void)skill_manifest_from_json(nonportable); },
      "resource paths must use the cross-platform portable grammar");
  }
  auto case_collision = nlohmann::json::parse(valid);
  auto duplicate_resource = case_collision["resources"][0];
  duplicate_resource["id"] = "secondary";
  duplicate_resource["path"] = "PROMPTS/MAIN.MD";
  case_collision["resources"].push_back(std::move(duplicate_resource));
  require_invalid([&] { (void)skill_manifest_from_json(case_collision); },
    "resource paths that collide on a supported filesystem are rejected");
  require_invalid([&] { (void)parse_skill_manifest(valid, { .max_manifest_bytes = 10 }); },
    "manifest byte limits are enforced before parsing");
  require_invalid(
    [&] {
      (void)parse_skill_manifest(
        R"({"schema_version":1,"id":"a","id":"b","version":"1.0.0","name":"n","description":"d"})");
    },
    "duplicate JSON object keys are rejected instead of using last-wins parsing");

  auto boolean_schemas = nlohmann::json::parse(valid);
  boolean_schemas["input_schema"] = true;
  boolean_schemas["output_schema"] = false;
  require(skill_manifest_from_json(boolean_schemas).descriptor.output_schema == false,
    "boolean JSON Schemas are supported by the manifest contract");
  auto unsupported_schema = nlohmann::json::parse(valid);
  unsupported_schema["input_schema"] = { { "type", "object" },
    { "patternProperties", nlohmann::json::object() } };
  require_invalid([&] { (void)skill_manifest_from_json(unsupported_schema); },
    "unsupported JSON Schema assertions fail closed");
  auto referenced_schema = nlohmann::json::parse(valid);
  referenced_schema["input_schema"] = {
    { "$defs", { { "name", { { "type", "string" } } } } },
    { "type", "object" },
    { "properties", { { "name", { { "$ref", "#/$defs/name" } } } } },
  };
  require(skill_manifest_from_json(referenced_schema).descriptor.input_schema.is_object(),
    "nested local references resolve against the complete schema root");
  auto opaque_tool_version = nlohmann::json::parse(valid);
  opaque_tool_version["requires"]["tools"][0]["exact_version"] = "1";
  require(skill_manifest_from_json(opaque_tool_version).tools.front().exact_version == "1",
    "tool versions remain opaque provider-defined identifiers");
  auto uppercase_digest = nlohmann::json::parse(valid);
  uppercase_digest["resources"][0]["sha256"] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
  require_invalid([&] { (void)skill_manifest_from_json(uppercase_digest); },
    "resource digests use one canonical lowercase representation");
  const auto invalid_result = try_skill_manifest_from_json(uppercase_digest);
  require(!invalid_result && invalid_result.error.code == skill_error_code::invalid_manifest,
    "manifest result APIs expose a stable typed error category");
}

void packages_are_immutable_and_registry_snapshots_are_stable() {
  static_assert(std::is_same_v<decltype(std::declval<const skill_package&>().manifest()),
    const skill_manifest&>);
  skill_registry registry;
  const auto first = package("com.example.skill", "1.0.0");
  require(require_registered(registry, first).status == skill_registration_status::inserted,
    "first registration reports insertion");
  require(require_registered(registry, first).status == skill_registration_status::unchanged,
    "registering the same package reports an idempotent outcome");
  require(require_registered(registry, first, skill_registration_policy::replace).status ==
            skill_registration_status::unchanged,
    "explicit replacement remains idempotent when immutable content is unchanged");
  require(require_registered(registry, package("com.example.skill", "1.0.0")).status ==
            skill_registration_status::unchanged,
    "registering equivalent immutable content is idempotent");
  require(registry.snapshot().size() == 1,
    "registering the same immutable package identity is idempotent");
  const auto conflict = registry.register_package(
    package("com.example.skill", "1.0.0", { dependency("com.example.other", "1.0.0", true) }));
  require(!conflict && conflict.error.code == skill_error_code::registration_conflict &&
            conflict.previous == first,
    "registration conflicts are returned as typed operational failures");
  const auto replacement =
    package("com.example.skill", "1.0.0", { dependency("com.example.other", "1.0.0", true) });
  const auto replaced =
    require_registered(registry, replacement, skill_registration_policy::replace);
  require(replaced.status == skill_registration_status::replaced && replaced.previous == first,
    "explicit replacement reports the previous immutable package");
  require(
    registry.snapshot().find("com.example.skill", semantic_version::parse("1.0.0")) == replacement,
    "package replacement requires an explicit strongly typed registration policy");
  const auto before = registry.snapshot();
  require_registered(registry, package("com.example.skill", "2.0.0"));
  const auto after = registry.snapshot();
  require(before.size() == 1 && after.size() == 2 &&
            before.select("com.example.skill", version_requirement::any())->descriptor().version ==
              semantic_version::parse("1.0.0") &&
            after.select("com.example.skill", version_requirement::any())->descriptor().version ==
              semantic_version::parse("2.0.0"),
    "copy-on-write registry snapshots remain immutable and select the highest stable version");

  require_registered(registry, package("com.example.build", "1.0.0+linux"));
  require_registered(registry, package("com.example.build", "1.0.0+windows"));
  require(registry.snapshot().find(
            "com.example.build", semantic_version::parse("1.0.0+linux")) != nullptr &&
            registry.snapshot().find(
              "com.example.build", semantic_version::parse("1.0.0+windows")) != nullptr,
    "registry identity preserves distinct build metadata variants");

  std::atomic<bool> consistent { true };
  std::thread reader([&] {
    for (int iteration = 0; iteration < 1000; ++iteration) {
      const auto snapshot = registry.snapshot();
      const auto count = snapshot.size();
      if (count < 4 || count > 24 || snapshot.packages().size() != count) {
        consistent = false;
      }
    }
  });
  for (int patch = 1; patch <= 20; ++patch) {
    if (!registry.register_package(
          package("com.example.concurrent", "1.0." + std::to_string(patch)))) {
      consistent = false;
    }
  }
  reader.join();
  require(consistent.load(), "registry snapshots support consistent lock-free concurrent reads");
}

void packages_enforce_exact_resource_snapshots() {
  constexpr auto empty_sha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  skill_resource_descriptor descriptor {
    .id = "main",
    .path = "SKILL.md",
    .kind = skill_resource_kind::instructions,
    .media_type = "text/markdown",
    .size = 0,
    .sha256 = empty_sha256,
  };
  skill_manifest manifest {
    .descriptor = {
      .id = "com.example.snapshot",
      .name = "Snapshot",
      .description = "Validates exact immutable resource bytes",
      .version = semantic_version::parse("1.0.0"),
    },
    .instruction_resources = { "main" },
    .resources = { descriptor },
  };
  require_invalid(
    [&] {
      (void)skill_package(manifest,
        {},
        {
          { "main", { .descriptor = descriptor, .content = "x", .sha256 = empty_sha256 } },
        });
    },
    "zero-byte resource declarations cannot bypass exact size validation");
  const auto invalid_package = skill_package::create(manifest,
    {},
    {
      { "main", { .descriptor = descriptor, .content = "x", .sha256 = empty_sha256 } },
    });
  require(!invalid_package && invalid_package.error.code == skill_error_code::invalid_package,
    "package construction provides a typed result API for external package data");
}

void resolver_is_deterministic_backtracking_and_dependency_first() {
  skill_registry registry;
  require_registered(registry, package("common", "1.5.0"));
  require_registered(registry, package("common", "2.5.0"));
  require_registered(
    registry, package("app", "1.0.0", { dependency("common", "^1.0.0") }));
  require_registered(
    registry, package("app", "2.0.0", { dependency("common", "^2.0.0") }));
  require_registered(
    registry, package("zother", "1.0.0", { dependency("common", "^1.0.0") }));

  const auto result = skill_resolver().resolve(
    registry.snapshot(), { .roots = { dependency("app", "*"), dependency("zother", "1.0.0") } });
  require(result && result.skills.size() == 3,
    "resolver finds one globally compatible version for every skill id");
  require(result.skills[0].package->descriptor().id == "common" &&
            result.skills[1].package->descriptor().id == "app" &&
            result.skills[1].package->descriptor().version == semantic_version::parse("1.0.0"),
    "resolver backtracks from the highest candidate and returns dependencies before consumers");

  const auto repeated = skill_resolver().resolve(
    registry.snapshot(), { .roots = { dependency("app", "*"), dependency("zother", "1.0.0") } });
  require(repeated && repeated.skills[0].package->descriptor().version ==
                        result.skills[0].package->descriptor().version,
    "resolver output is deterministic across repeated snapshots");
}

void resolver_reports_cycles_conflicts_optional_dependencies_and_limits() {
  skill_registry registry;
  require_registered(registry, package("a", "1.0.0", { dependency("b", "1.0.0") }));
  require_registered(registry, package("b", "1.0.0", { dependency("a", "1.0.0") }));
  const auto cycle = skill_resolver().resolve(
    registry.snapshot(), { .roots = { dependency("a", "1.0.0"), dependency("b", "1.0.0") } });
  require(
    !cycle && !cycle.diagnostics.empty() && cycle.diagnostics.back().code == "dependency_cycle",
    "dependency cycles are rejected with a structured diagnostic");

  skill_registry optional_registry;
  require_registered(optional_registry,
    package("root", "1.0.0", { dependency("missing", "^1.0.0", true) }));
  const auto optional = skill_resolver().resolve(
    optional_registry.snapshot(), { .roots = { dependency("root", "1.0.0") } });
  require(optional && optional.skills.size() == 1 && !optional.diagnostics.empty() &&
            optional.diagnostics.front().code == "optional_dependency_skipped",
    "unavailable optional dependencies are skipped and remain observable");

  const auto limited = skill_resolver().resolve(optional_registry.snapshot(),
    { .roots = { dependency("root", "1.0.0") }, .limits = { .max_skills = 1 } });
  require(static_cast<bool>(limited),
    "a resolution whose selected skill count equals the configured maximum is accepted");
  const auto disabled = skill_resolver().resolve(optional_registry.snapshot(),
    { .roots = { dependency("root", "1.0.0") }, .limits = { .max_skills = 0 } });
  require(!disabled && disabled.diagnostics.front().code == "resolution_limit_exceeded",
    "invalid resolution safety limits fail closed");
}

} // namespace

int main() {
  semver_is_strict_and_follows_precedence_rules();
  version_requirements_cover_exact_ranges_caret_and_tilde();
  manifest_parser_is_strict_and_bounded();
  packages_are_immutable_and_registry_snapshots_are_stable();
  packages_enforce_exact_resource_snapshots();
  resolver_is_deterministic_backtracking_and_dependency_first();
  resolver_reports_cycles_conflicts_optional_dependencies_and_limits();
  return 0;
}
