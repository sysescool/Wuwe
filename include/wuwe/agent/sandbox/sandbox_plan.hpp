#ifndef WUWE_AGENT_SANDBOX_SANDBOX_PLAN_HPP
#define WUWE_AGENT_SANDBOX_SANDBOX_PLAN_HPP

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <wuwe/agent/sandbox/sandbox_policy.hpp>

namespace wuwe::agent::sandbox {

class sandbox_plan {
public:
  virtual ~sandbox_plan() = default;

  [[nodiscard]] const std::string& backend_name() const noexcept {
    return backend_name_;
  }

  [[nodiscard]] sandbox_platform platform() const noexcept {
    return platform_;
  }

  [[nodiscard]] const sandbox_policy& policy() const noexcept {
    return policy_;
  }

  [[nodiscard]] const sandbox_enforcement_contract& enforcement() const noexcept {
    return enforcement_;
  }

  [[nodiscard]] const std::map<std::string, std::string>& metadata() const noexcept {
    return metadata_;
  }

protected:
  sandbox_plan(std::string backend_name, sandbox_platform platform, sandbox_policy policy,
    sandbox_enforcement_contract enforcement, std::map<std::string, std::string> metadata = {});

private:
  std::string backend_name_;
  sandbox_platform platform_ { sandbox_platform::unknown };
  sandbox_policy policy_;
  sandbox_enforcement_contract enforcement_;
  std::map<std::string, std::string> metadata_;
};

enum class sandbox_compile_error {
  none,
  invalid_policy,
  backend_unavailable,
  unsupported_isolation,
  unsupported_policy,
};

struct sandbox_compile_result {
  std::shared_ptr<const sandbox_plan> plan;
  sandbox_compile_error error { sandbox_compile_error::none };
  std::string message;
  std::vector<std::string> blockers;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == sandbox_compile_error::none && plan != nullptr;
  }
};

[[nodiscard]] std::string to_string(sandbox_compile_error error);

[[nodiscard]] sandbox_compile_result compile_sandbox_policy(const sandbox_policy& policy,
  const sandbox_backend_info& backend, sandbox_platform platform = current_sandbox_platform());

} // namespace wuwe::agent::sandbox

#endif // WUWE_AGENT_SANDBOX_SANDBOX_PLAN_HPP
