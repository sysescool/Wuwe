#ifndef WUWE_AGENT_FILESYSTEM_FILESYSTEM_TOOLS_HPP
#define WUWE_AGENT_FILESYSTEM_FILESYSTEM_TOOLS_HPP

#include <cstddef>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <wuwe/agent/filesystem/filesystem_runtime.hpp>
#include <wuwe/agent/llm/llm_types.h>
#include <wuwe/agent/tools/tool_contract.hpp>

namespace wuwe::agent::filesystem {

struct filesystem_tool_options {
  std::string name_prefix;
  std::size_t max_arguments_bytes { 256 * 1024 };
};

class filesystem_tool_provider {
public:
  explicit filesystem_tool_provider(
    filesystem_runtime& runtime, filesystem_tool_options options = {});

  [[nodiscard]] std::vector<llm_tool> tools() const;
  [[nodiscard]] llm_tool_result invoke(
    const std::string& name, const std::string& arguments_json) const;
  [[nodiscard]] llm_tool_result invoke(
    const std::string& name, const std::string& arguments_json, std::stop_token stop_token) const;

private:
  [[nodiscard]] std::string tool_name(std::string_view base) const;

  filesystem_runtime& runtime_;
  filesystem_tool_options options_;
};

} // namespace wuwe::agent::filesystem

#endif // WUWE_AGENT_FILESYSTEM_FILESYSTEM_TOOLS_HPP
