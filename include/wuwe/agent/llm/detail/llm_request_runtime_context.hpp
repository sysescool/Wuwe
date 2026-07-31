#ifndef WUWE_AGENT_LLM_DETAIL_LLM_REQUEST_RUNTIME_CONTEXT_HPP
#define WUWE_AGENT_LLM_DETAIL_LLM_REQUEST_RUNTIME_CONTEXT_HPP

#include <memory>

namespace wuwe::agent::llm::detail {

struct llm_dispatch_lineage;

// Framework-owned, process-local request state. The type is intentionally
// opaque to public request consumers and is never part of durable serialization.
struct llm_request_runtime_context {
  std::shared_ptr<const llm_dispatch_lineage> dispatch_lineage;
};

} // namespace wuwe::agent::llm::detail

#endif // WUWE_AGENT_LLM_DETAIL_LLM_REQUEST_RUNTIME_CONTEXT_HPP
