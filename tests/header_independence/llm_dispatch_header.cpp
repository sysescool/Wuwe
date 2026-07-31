#include <memory>

#include <wuwe/agent/llm/dispatching_llm_client.hpp>

bool llm_dispatch_header_is_independent() {
  auto registry = std::make_shared<wuwe::agent::llm::llm_client_registry>();
  wuwe::agent::llm::dispatching_llm_client client(std::move(registry));
  return client.registry() != nullptr;
}
