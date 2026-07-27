#ifndef WUWE_AGENT_APPROVAL_APPROVAL_CONTEXT_HPP
#define WUWE_AGENT_APPROVAL_APPROVAL_CONTEXT_HPP

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/approval/approval.hpp>
#include <wuwe/agent/core/execution_context_projection.hpp>

namespace wuwe::agent::approval {

[[nodiscard]] inline approval_request make_approval_request(
  const core::agent_execution_context& context,
  std::string id,
  std::string summary,
  std::vector<capability::capability_request> capabilities = {},
  std::map<std::string, std::string> metadata = {}) {
  core::apply_execution_context_attributes(metadata, context);
  return {
    .id = std::move(id),
    .summary = std::move(summary),
    .capabilities = std::move(capabilities),
    .metadata = std::move(metadata),
  };
}

} // namespace wuwe::agent::approval

#endif // WUWE_AGENT_APPROVAL_APPROVAL_CONTEXT_HPP
