#ifndef WUWE_AGENT_PROCESS_PROCESS_BACKEND_HPP
#define WUWE_AGENT_PROCESS_PROCESS_BACKEND_HPP

#include <stop_token>

#include <wuwe/agent/process/process_core.hpp>
#include <wuwe/agent/sandbox/sandbox.hpp>

namespace wuwe::agent::process {

class process_backend {
public:
  virtual ~process_backend() = default;

  [[nodiscard]] virtual sandbox::sandbox_backend_info info() const = 0;
  [[nodiscard]] virtual process_result run(
    const process_request& request, std::stop_token stop_token) = 0;
};

} // namespace wuwe::agent::process

#endif // WUWE_AGENT_PROCESS_PROCESS_BACKEND_HPP
