#include <wuwe/version.hpp>

bool version_header_is_independent() {
  static_assert(wuwe::framework_version_major == 1);
  static_assert(wuwe::framework_version_minor == 0);
  static_assert(wuwe::framework_version_patch == 0);
  return wuwe::framework_version == "1.0.0";
}
