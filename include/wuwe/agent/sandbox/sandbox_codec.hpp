#ifndef WUWE_AGENT_SANDBOX_SANDBOX_CODEC_HPP
#define WUWE_AGENT_SANDBOX_SANDBOX_CODEC_HPP

#include <nlohmann/json.hpp>

#include <wuwe/agent/sandbox/sandbox_policy.hpp>

namespace wuwe::agent::sandbox {

[[nodiscard]] nlohmann::json sandbox_policy_to_json(const sandbox_policy& policy);
[[nodiscard]] sandbox_policy sandbox_policy_from_json(const nlohmann::json& encoded);

} // namespace wuwe::agent::sandbox

#endif // WUWE_AGENT_SANDBOX_SANDBOX_CODEC_HPP
