#ifndef WUWE_AGENT_A2A_A2A_TRANSPORT_HPP
#define WUWE_AGENT_A2A_A2A_TRANSPORT_HPP

#include <memory>
#include <stop_token>
#include <string>

#include <nlohmann/json.hpp>

#include <wuwe/agent/a2a/a2a_types.hpp>

namespace wuwe::agent::a2a {

struct transport_capabilities {
  bool cooperative_cancellation { false };
  bool streaming { false };
  bool concurrent_invocation { false };
};

class transport {
public:
  virtual ~transport() = default;

  virtual rpc_result invoke(
    std::string method,
    nlohmann::json params,
    std::stop_token stop_token = {}) = 0;

  virtual result<agent_card> discover(std::stop_token stop_token = {}) = 0;

  [[nodiscard]] virtual transport_capabilities capabilities() const noexcept {
    return {};
  }
};

} // namespace wuwe::agent::a2a

#endif // WUWE_AGENT_A2A_A2A_TRANSPORT_HPP
