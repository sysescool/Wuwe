#ifndef WUWE_AGENT_SANDBOX_SANDBOX_BACKEND_HPP
#define WUWE_AGENT_SANDBOX_SANDBOX_BACKEND_HPP

#include <utility>

#include <wuwe/agent/sandbox/sandbox_plan.hpp>

namespace wuwe::agent::sandbox {

struct sandbox_host_capabilities {
  sandbox_platform platform { sandbox_platform::unknown };
  sandbox_backend_info backend;
  std::vector<std::string> blockers;
};

class sandbox_backend {
public:
  virtual ~sandbox_backend() = default;

  [[nodiscard]] virtual sandbox_backend_info info() const = 0;
  [[nodiscard]] virtual sandbox_platform platform() const noexcept = 0;
  [[nodiscard]] virtual sandbox_compile_result compile(const sandbox_policy& policy) const = 0;

  [[nodiscard]] virtual sandbox_host_capabilities probe() const {
    auto backend = info();
    sandbox_host_capabilities capabilities {
      .platform = platform(),
      .backend = std::move(backend),
    };
    if (!capabilities.backend.available && !capabilities.backend.unavailable_reason.empty()) {
      capabilities.blockers.push_back(capabilities.backend.unavailable_reason);
    }
    return capabilities;
  }
};

} // namespace wuwe::agent::sandbox

#endif // WUWE_AGENT_SANDBOX_SANDBOX_BACKEND_HPP
