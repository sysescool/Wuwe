#ifndef WUWE_AGENT_PROCESS_PROCESS_POLICY_HPP
#define WUWE_AGENT_PROCESS_PROCESS_POLICY_HPP

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <wuwe/agent/process/process_core.hpp>

namespace wuwe::agent::process {

struct process_policy {
  std::filesystem::path working_directory_root;
  std::filesystem::path default_workdir { "." };
  std::vector<std::filesystem::path> allowed_executables;
  std::vector<std::filesystem::path> executable_search_paths;
  std::map<std::string, std::string> base_environment;
  std::set<std::string> allowed_environment_overrides;
  bool inherit_parent_environment { false };
  bool allow_shell { false };
  bool require_approval_for_process { false };
  bool require_approval_for_shell { true };
  std::filesystem::path shell_executable;
  process_limits max_limits;
};

} // namespace wuwe::agent::process

#endif // WUWE_AGENT_PROCESS_PROCESS_POLICY_HPP
