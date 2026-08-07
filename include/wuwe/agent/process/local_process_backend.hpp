#ifndef WUWE_AGENT_PROCESS_LOCAL_PROCESS_BACKEND_HPP
#define WUWE_AGENT_PROCESS_LOCAL_PROCESS_BACKEND_HPP

#include <chrono>
#include <memory>

#include <wuwe/agent/process/process_backend.hpp>

namespace wuwe::agent::process {

struct local_process_backend_config {
  std::chrono::milliseconds cancellation_poll_interval { 25 };
  bool use_process_tree { true };
};

class local_process_backend final : public process_backend {
public:
  explicit local_process_backend(local_process_backend_config config = {});

  [[nodiscard]] sandbox::sandbox_backend_info info() const override;
  [[nodiscard]] process_result run(
    const process_request& request, std::stop_token stop_token) override;

private:
  local_process_backend_config config_;
};

[[nodiscard]] std::unique_ptr<process_backend> make_local_process_backend(
  local_process_backend_config config = {});

} // namespace wuwe::agent::process

#endif // WUWE_AGENT_PROCESS_LOCAL_PROCESS_BACKEND_HPP
