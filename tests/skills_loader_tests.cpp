#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

#include <nlohmann/json.hpp>

#include <wuwe/agent/skills/directory_skill_loader.hpp>
#include <wuwe/common/sha256.hpp>

namespace {

using namespace wuwe;
using namespace wuwe::agent;
using namespace wuwe::agent::skills;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class temporary_directory {
public:
  temporary_directory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ =
      std::filesystem::temp_directory_path() / ("wuwe-skills-loader-" + std::to_string(nonce));
    std::filesystem::create_directories(path_);
    path_ = std::filesystem::canonical(path_);
  }

  ~temporary_directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!output) {
    throw std::runtime_error("failed to prepare skill loader fixture");
  }
}

std::string bare_sha256(std::string_view content) {
  return common::sha256_hex(content);
}

nlohmann::json manifest_with_resource(
  std::string path, std::string_view content, std::string id = "instructions") {
  return {
    { "schema_version", 1 },
    { "id", "test.skill" },
    { "version", "1.2.3" },
    { "name", "Test skill" },
    { "description", "A skill loader test package" },
    { "instructions", nlohmann::json::array({ id }) },
    { "resources",
      nlohmann::json::array({
        {
          { "id", std::move(id) },
          { "path", std::move(path) },
          { "kind", "instructions" },
          { "media_type", "text/markdown" },
          { "size", content.size() },
          { "sha256", bare_sha256(content) },
        },
      }) },
  };
}

void write_manifest(const std::filesystem::path& package, const nlohmann::json& manifest) {
  write_file(package / "skill.json", manifest.dump());
}

directory_skill_loader loader_for(
  const std::filesystem::path& root, std::size_t max_package_bytes = 8 * 1024 * 1024) {
  return directory_skill_loader({
    .root = root,
    .max_package_bytes = max_package_bytes,
  });
}

void test_sha256_known_vectors() {
  require(bare_sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    "SHA-256 empty vector mismatch");
  require(bare_sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    "SHA-256 abc vector mismatch");

  common::sha256 incremental;
  incremental.update("a");
  const auto first_snapshot = incremental.hex_digest();
  incremental.update("bc");
  require(first_snapshot == bare_sha256("a"), "SHA-256 digest must not consume state");
  require(incremental.hex_digest() == bare_sha256("abc"),
    "incremental SHA-256 must match one-shot hashing");
  require(bare_sha256(std::string(1'000'000, 'a')) ==
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
    "SHA-256 multi-block vector mismatch");
}

void test_valid_package_is_frozen_and_untrusted() {
  temporary_directory fixture;
  const auto package_path = fixture.path() / "demo";
  const std::string instructions = "Review the input without executing scripts.";
  write_file(package_path / "instructions.md", instructions);
  write_manifest(package_path, manifest_with_resource("instructions.md", instructions));

  observability::in_memory_event_sink events;
  auto result =
    loader_for(fixture.path()).load("demo", &events, { .trace_id = "trace-1", .run_id = "run-1" });
  require(static_cast<bool>(result), "valid package should load: " + result.message);
  require(result.package->provenance().origin == skill_origin_kind::local,
    "directory package must have local origin");
  require(result.package->provenance().trust == core::content_trust_level::retrieved_untrusted,
    "local skill package must be untrusted by default");
  require(result.package->provenance().content_sha256.size() == 64,
    "package digest must be a canonical SHA-256");
  const auto* resource = result.package->find_resource("instructions");
  require(resource && resource->content == instructions, "resource must be loaded eagerly");
  require(resource->sha256 == bare_sha256(instructions),
    "loaded resource must retain its verified digest");

  write_file(package_path / "instructions.md", "tampered after loading");
  require(resource->content == instructions, "loaded package contents must be immutable snapshots");

  const auto captured = events.events();
  require(captured.size() == 2 && captured.front().name == "skill.load.started" &&
            captured.back().name == "skill.load.succeeded",
    "loader should emit lifecycle events");
  for (const auto& event : captured) {
    require(event.data.is_null(), "skill telemetry must not carry content data");
    for (const auto& [key, value] : event.attributes) {
      (void)key;
      require(value != instructions, "skill telemetry must not expose resource content");
    }
  }
}

void test_integrity_and_budget_failures() {
  temporary_directory fixture;
  const auto package_path = fixture.path() / "demo";
  write_file(package_path / "instructions.md", "actual");

  auto mismatch = manifest_with_resource("instructions.md", "different");
  mismatch["resources"][0]["size"] = 6;
  write_manifest(package_path, mismatch);
  auto result = loader_for(fixture.path()).load("demo");
  require(result.error == skill_load_error_code::resource_digest_mismatch,
    "digest mismatch must be classified");

  auto uppercase = manifest_with_resource("instructions.md", "actual");
  auto digest = uppercase["resources"][0]["sha256"].get<std::string>();
  digest[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(digest[0])));
  if (digest[0] >= '0' && digest[0] <= '9') {
    digest[0] = 'A';
  }
  uppercase["resources"][0]["sha256"] = digest;
  write_manifest(package_path, uppercase);
  result = loader_for(fixture.path()).load("demo");
  require(result.error == skill_load_error_code::manifest_invalid,
    "manifest digests must use canonical lowercase encoding");

  auto missing_size = manifest_with_resource("instructions.md", "actual");
  missing_size["resources"][0].erase("size");
  write_manifest(package_path, missing_size);
  result = loader_for(fixture.path()).load("demo");
  require(result.error == skill_load_error_code::manifest_invalid,
    "resource integrity metadata must be explicit even for empty resources");

  write_manifest(package_path, manifest_with_resource("instructions.md", "actual"));
  result = loader_for(fixture.path(), 16).load("demo");
  require(result.error == skill_load_error_code::invalid_options,
    "incoherent byte limits must be rejected before loading");

  directory_skill_loader resource_budget_loader({
    .root = fixture.path(),
    .max_manifest_bytes = 1024,
    .max_resource_bytes = 4,
    .max_package_bytes = 2048,
  });
  result = resource_budget_loader.load("demo");
  require(result.error == skill_load_error_code::resource_too_large,
    "per-resource byte budgets must be enforced before allocation");

  const auto manifest_bytes =
    static_cast<std::size_t>(std::filesystem::file_size(package_path / "skill.json"));
  directory_skill_loader package_budget_loader({
    .root = fixture.path(),
    .max_manifest_bytes = manifest_bytes,
    .max_resource_bytes = 6,
    .max_package_bytes = manifest_bytes + 5,
  });
  result = package_budget_loader.load("demo");
  require(result.error == skill_load_error_code::package_too_large,
    "aggregate package byte budgets must include manifest and resource bytes");

  auto wrong_size = manifest_with_resource("instructions.md", "actual");
  wrong_size["resources"][0]["size"] = 5;
  write_manifest(package_path, wrong_size);
  result = loader_for(fixture.path()).load("demo");
  require(result.error == skill_load_error_code::resource_size_mismatch,
    "declared resource size mismatches must be classified");
}

void test_strict_paths_and_undeclared_files() {
  temporary_directory fixture;
  const auto package_path = fixture.path() / "demo";
  write_file(package_path / "instructions.md", "safe");
  write_manifest(package_path, manifest_with_resource("instructions.md", "safe"));
  auto loader = loader_for(fixture.path());

  require(loader.load("../demo").error == skill_load_error_code::invalid_path,
    "parent traversal must be rejected");
  require(loader.load("demo\\nested").error == skill_load_error_code::invalid_path,
    "backslash paths must be rejected");
  require(loader.load(package_path).error == skill_load_error_code::invalid_path,
    "absolute package paths must be rejected");
  const auto invalid_result = loader.load(std::string(1025, 'x'));
  require(invalid_result.error == skill_load_error_code::invalid_path &&
            !invalid_result.diagnostics.empty(),
    "path errors must return a structured diagnostic (error=" +
      std::string(to_string(invalid_result.error)) +
      ", diagnostics=" + std::to_string(invalid_result.diagnostics.size()) + ")");

  auto reserved = manifest_with_resource("CON.md", "safe");
  write_manifest(package_path, reserved);
  require(loader.load("demo").error == skill_load_error_code::manifest_invalid,
    "non-portable manifest resource paths must be rejected on every platform");

  write_manifest(package_path, manifest_with_resource("instructions.md", "safe"));
  write_file(package_path / "hidden.txt", "not declared");
  require(loader.load("demo").error == skill_load_error_code::undeclared_resource,
    "undeclared package files must be rejected so the digest covers the full package");
}

void test_case_collisions_and_hard_links() {
  temporary_directory fixture;
  const auto package_path = fixture.path() / "demo";
  write_file(package_path / "Readme.md", "safe");
  auto manifest = manifest_with_resource("Readme.md", "safe", "one");
  auto duplicate = manifest["resources"][0];
  duplicate["id"] = "two";
  duplicate["path"] = "README.md";
  manifest["resources"].push_back(duplicate);
  manifest["instructions"].push_back("two");
  write_manifest(package_path, manifest);
  require(loader_for(fixture.path()).load("demo").error == skill_load_error_code::manifest_invalid,
    "case-insensitive resource path collisions must be rejected");

  const auto external_link = fixture.path() / "external-link.md";
  std::error_code error;
  std::filesystem::create_hard_link(package_path / "Readme.md", external_link, error);
  if (!error) {
    write_manifest(package_path, manifest_with_resource("Readme.md", "safe"));
    require(loader_for(fixture.path()).load("demo").error == skill_load_error_code::hard_link,
      "multiply-linked resources must be rejected by default");
  }
}

void test_reparse_points_are_rejected() {
  temporary_directory fixture;
  const auto package_path = fixture.path() / "demo";
  const auto target = fixture.path() / "target.md";
  write_file(target, "safe");
  std::filesystem::create_directories(package_path);
  std::error_code error;
  std::filesystem::create_symlink(target, package_path / "instructions.md", error);
  if (error) {
    return; // Creating symlinks can require an elevated Windows token.
  }
  write_manifest(package_path, manifest_with_resource("instructions.md", "safe"));
  require(loader_for(fixture.path()).load("demo").error == skill_load_error_code::reparse_point,
    "symbolic links and reparse points must be rejected");
}

void test_script_resources_are_data_only() {
  temporary_directory fixture;
  const auto package_path = fixture.path() / "demo";
  const auto marker = fixture.path() / "must-not-exist";
  const auto script = "Set-Content -Path '" + marker.string() + "' -Value unsafe";
  write_file(package_path / "scripts" / "setup.ps1", script);
  auto manifest = manifest_with_resource("scripts/setup.ps1", script);
  manifest["resources"][0]["kind"] = "script";
  manifest["instructions"] = nlohmann::json::array();
  write_manifest(package_path, manifest);
  const auto result = loader_for(fixture.path()).load("demo");
  require(static_cast<bool>(result), "declared script resource should load as inert data");
  require(!std::filesystem::exists(marker), "skill loading must never execute script resources");
}

} // namespace

int main() {
  static_assert(std::is_same_v<skill_package_ptr, std::shared_ptr<const skill_package>>);
  try {
    test_sha256_known_vectors();
    test_valid_package_is_frozen_and_untrusted();
    test_integrity_and_budget_failures();
    test_strict_paths_and_undeclared_files();
    test_case_collisions_and_hard_links();
    test_reparse_points_are_rejected();
    test_script_resources_are_data_only();
    std::cout << "skills loader tests passed\n";
    return 0;
  }
  catch (const std::exception& exception) {
    std::cerr << "skills loader tests failed: " << exception.what() << '\n';
    return 1;
  }
}
