#include <wuwe/agent/filesystem/filesystem.hpp>

bool filesystem_tools_header_is_independent() {
  return wuwe::agent::filesystem::filesystem_policy {}.allow_read;
}
