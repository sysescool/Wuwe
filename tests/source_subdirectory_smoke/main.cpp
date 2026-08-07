#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/agent/skills/skills.hpp>
#include <wuwe/version.hpp>

int main() {
  static_assert(wuwe::framework_version_major == 1);
  static_assert(wuwe::framework_version_minor == 0);
  const wuwe::agent::skills::skill_registry registry;
  return wuwe::framework_version == "1.0.0" && registry.snapshot().empty() ? 0 : 1;
}
