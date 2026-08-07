#include <wuwe/agent/process/process_core.hpp>

namespace wuwe::agent::process {

std::string to_string(process_termination_reason reason) {
  switch (reason) {
    case process_termination_reason::exited:
      return "exited";
    case process_termination_reason::timed_out:
      return "timed_out";
    case process_termination_reason::cancelled:
      return "cancelled";
    case process_termination_reason::launch_failed:
      return "launch_failed";
    case process_termination_reason::policy_denied:
      return "policy_denied";
    case process_termination_reason::approval_denied:
      return "approval_denied";
    case process_termination_reason::backend_error:
      return "backend_error";
  }
  return "backend_error";
}

} // namespace wuwe::agent::process
