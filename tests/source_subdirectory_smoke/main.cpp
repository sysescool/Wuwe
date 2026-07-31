#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/version.hpp>

int main() {
  static_assert(wuwe::framework_version_major == 1);
  static_assert(wuwe::framework_version_minor == 0);
  return wuwe::framework_version == "1.0.0" ? 0 : 1;
}
