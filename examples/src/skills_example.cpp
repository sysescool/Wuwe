#include <filesystem>
#include <iostream>

#include <wuwe/agent/skills/skills.hpp>

int main(int argc, char** argv) {
  namespace skills = wuwe::agent::skills;

  const auto root = std::filesystem::absolute(
    argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("examples/skills"));
  skills::directory_skill_loader loader({ .root = root });
  const auto loaded = loader.load("review");
  if (!loaded) {
    std::cerr << "load failed [" << skills::to_string(loaded.error) << "]: " << loaded.message
              << '\n';
    return 1;
  }

  skills::skill_registry registry;
  registry.register_package(loaded.package);

  const auto resolved = skills::skill_resolver().resolve(registry.snapshot(),
    {
      .roots = { {
        .id = "org.wuwe.examples.review",
        .version = skills::version_requirement::parse("^1.0.0"),
      } },
    });
  if (!resolved) {
    std::cerr << "dependency resolution failed\n";
    return 1;
  }

  const auto activated = skills::skill_activator().activate({ .resolution = resolved });
  if (!activated) {
    std::cerr << "activation failed\n";
    return 1;
  }

  std::cout << "activated " << activated.packages.front()->descriptor().id << '@'
            << activated.packages.front()->descriptor().version.string() << '\n'
            << "fingerprint: " << activated.fingerprint << '\n'
            << "instruction trust: "
            << wuwe::agent::core::to_string(activated.instructions.front().provenance.trust)
            << '\n';
}
