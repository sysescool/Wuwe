#ifndef WUWE_AGENT_PROCESS_PROCESS_CORE_HPP
#define WUWE_AGENT_PROCESS_PROCESS_CORE_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wuwe::agent::process {

enum class process_termination_reason {
  exited,
  timed_out,
  cancelled,
  launch_failed,
  policy_denied,
  approval_denied,
  backend_error,
};

struct process_limits {
  std::chrono::milliseconds timeout { 30000 };
  std::size_t max_stdout_bytes { 1024 * 1024 };
  std::size_t max_stderr_bytes { 1024 * 1024 };
  std::size_t max_stdin_bytes { 1024 * 1024 };
  std::size_t max_argument_bytes { 256 * 1024 };
  std::size_t max_argument_count { 256 };
  std::size_t max_environment_bytes { 64 * 1024 };
  std::size_t max_environment_count { 256 };
  std::chrono::milliseconds max_cpu_time { 0 };
};

struct process_request {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  std::string stdin_text;
  std::filesystem::path workdir;
  std::map<std::string, std::string> environment;
  bool inherit_parent_environment { false };
  process_limits limits;
  std::map<std::string, std::string> metadata;
};

struct shell_request {
  std::string command;
  std::string stdin_text;
  std::filesystem::path workdir;
  std::map<std::string, std::string> environment;
  process_limits limits;
  std::map<std::string, std::string> metadata;
};

struct process_result {
  process_termination_reason termination_reason { process_termination_reason::backend_error };
  std::optional<int> exit_code;
  std::optional<std::uint64_t> process_id;
  bool stdout_truncated { false };
  bool stderr_truncated { false };
  std::string stdout_text;
  std::string stderr_text;
  std::string error_message;
  std::chrono::milliseconds elapsed { 0 };
  std::map<std::string, std::string> metadata;

  [[nodiscard]] bool launched() const noexcept {
    return termination_reason == process_termination_reason::exited ||
           termination_reason == process_termination_reason::timed_out ||
           termination_reason == process_termination_reason::cancelled;
  }
};

[[nodiscard]] std::string to_string(process_termination_reason reason);

} // namespace wuwe::agent::process

#endif // WUWE_AGENT_PROCESS_PROCESS_CORE_HPP
