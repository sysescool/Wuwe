#include <wuwe/agent/skills/skill_types.hpp>

namespace wuwe::agent::skills {

const char* to_string(skill_error_code value) noexcept {
  switch (value) {
    case skill_error_code::none:
      return "none";
    case skill_error_code::invalid_manifest:
      return "invalid_manifest";
    case skill_error_code::invalid_package:
      return "invalid_package";
    case skill_error_code::invalid_registration:
      return "invalid_registration";
    case skill_error_code::registration_conflict:
      return "registration_conflict";
  }
  return "unknown";
}

} // namespace wuwe::agent::skills
