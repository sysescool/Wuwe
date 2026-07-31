#ifndef WUWE_AGENT_CORE_FILESYSTEM_HPP
#define WUWE_AGENT_CORE_FILESYSTEM_HPP

#include <filesystem>
#include <string>

namespace wuwe::agent::core {

[[nodiscard]] inline std::string filesystem_path_to_utf8(const std::filesystem::path& path) {
  const auto encoded = path.u8string();
  return std::string(encoded.begin(), encoded.end());
}

} // namespace wuwe::agent::core

#endif // WUWE_AGENT_CORE_FILESYSTEM_HPP
