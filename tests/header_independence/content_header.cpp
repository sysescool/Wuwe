#include <wuwe/agent/core/content.hpp>

bool content_header_is_independent() {
  return wuwe::agent::core::trusted_for_system_message(
    wuwe::agent::core::content_trust_level::system_trusted);
}
