#ifndef WUWE_AGENT_PROCESS_PROCESS_TOOLS_HPP
#define WUWE_AGENT_PROCESS_PROCESS_TOOLS_HPP

#include <cstddef>
#include <stop_token>
#include <string>
#include <vector>

#include <wuwe/agent/llm/llm_types.h>
#include <wuwe/agent/process/process_runtime.hpp>
#include <wuwe/agent/tools/tool_contract.hpp>

namespace wuwe::agent::process {

struct process_tool_options {
  std::string process_tool_name { "run_process" };
  std::string shell_tool_name { "run_shell" };
  bool expose_shell_tool { false };
  std::size_t max_arguments_json_bytes { 512 * 1024 };
};

class process_tool_provider {
public:
  explicit process_tool_provider(
    process_runtime& runtime,
    process_tool_options options = {});

  [[nodiscard]] std::vector<llm_tool> tools() const;
  [[nodiscard]] llm_tool_result invoke(
    const std::string& name,
    const std::string& arguments_json) const;
  [[nodiscard]] llm_tool_result invoke(
    const std::string& name,
    const std::string& arguments_json,
    std::stop_token stop_token) const;

private:
  process_runtime& runtime_;
  process_tool_options options_;
};

} // namespace wuwe::agent::process

#endif // WUWE_AGENT_PROCESS_PROCESS_TOOLS_HPP
