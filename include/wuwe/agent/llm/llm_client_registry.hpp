#ifndef WUWE_AGENT_LLM_LLM_CLIENT_REGISTRY_HPP
#define WUWE_AGENT_LLM_LLM_CLIENT_REGISTRY_HPP

#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wuwe/agent/llm/llm_client.h>

namespace wuwe::agent::llm {

class llm_client_registry;

// Implemented by framework clients that own or traverse registries. The
// registry uses this graph-validation contract to reject strong-ownership
// cycles before a binding is committed. Validation is allowed to throw so an
// allocation or custom traversal failure aborts the mutation atomically rather
// than accepting an unverified ownership edge.
class llm_client_registry_dependency {
public:
  virtual ~llm_client_registry_dependency() = default;

  [[nodiscard]] virtual bool references_registry(
    const llm_client_registry* registry, std::vector<const void*>& traversal) const = 0;
};

struct llm_client_binding {
  std::string provider;
  std::shared_ptr<::wuwe::llm_client> client;
  std::map<std::string, std::string> metadata;
};

class llm_client_registry {
public:
  llm_client_registry() = default;

  explicit llm_client_registry(std::vector<llm_client_binding> bindings) {
    for (auto& binding : bindings) {
      add(std::move(binding));
    }
  }

  llm_client_registry(const llm_client_registry&) = delete;
  llm_client_registry& operator=(const llm_client_registry&) = delete;

  llm_client_registry& add(llm_client_binding binding) {
    std::scoped_lock graph_lock(graph_mutation_mutex());
    validate(binding);
    reject_ownership_cycle(binding);
    const auto provider = binding.provider;
    std::unique_lock lock(mutex_);
    const auto [_, inserted] = bindings_.emplace(provider, std::move(binding));
    if (!inserted) {
      throw std::invalid_argument("duplicate LLM client provider: " + provider);
    }
    return *this;
  }

  [[nodiscard]] std::shared_ptr<::wuwe::llm_client> replace(llm_client_binding binding) {
    std::scoped_lock graph_lock(graph_mutation_mutex());
    validate(binding);
    reject_ownership_cycle(binding);
    const auto provider = binding.provider;
    std::unique_lock lock(mutex_);
    const auto found = bindings_.find(provider);
    if (found == bindings_.end()) {
      throw std::invalid_argument("LLM client provider is not registered: " + provider);
    }
    auto previous = found->second.client;
    found->second = std::move(binding);
    return previous;
  }

  [[nodiscard]] std::shared_ptr<::wuwe::llm_client> remove(std::string_view provider) {
    std::scoped_lock graph_lock(graph_mutation_mutex());
    std::unique_lock lock(mutex_);
    const auto found = bindings_.find(provider);
    if (found == bindings_.end()) {
      return {};
    }
    auto previous = std::move(found->second.client);
    bindings_.erase(found);
    return previous;
  }

  [[nodiscard]] std::shared_ptr<::wuwe::llm_client> find(std::string_view provider) const {
    std::shared_lock lock(mutex_);
    const auto found = bindings_.find(provider);
    return found == bindings_.end() ? std::shared_ptr<::wuwe::llm_client> {} : found->second.client;
  }

  [[nodiscard]] std::vector<llm_client_binding> snapshot() const {
    std::shared_lock lock(mutex_);
    std::vector<llm_client_binding> output;
    output.reserve(bindings_.size());
    for (const auto& [_, binding] : bindings_) {
      output.push_back(binding);
    }
    return output;
  }

  [[nodiscard]] std::size_t size() const {
    std::shared_lock lock(mutex_);
    return bindings_.size();
  }

  [[nodiscard]] bool empty() const {
    return size() == 0;
  }

private:
  static std::mutex& graph_mutation_mutex() {
    static std::mutex mutex;
    return mutex;
  }

  static void validate(const llm_client_binding& binding) {
    if (binding.provider.empty()) {
      throw std::invalid_argument("LLM client binding requires a provider id");
    }
    if (!binding.client) {
      throw std::invalid_argument(
        "LLM client binding requires a client for provider: " + binding.provider);
    }
  }

  void reject_ownership_cycle(const llm_client_binding& binding) const {
    const auto* dependency =
      dynamic_cast<const llm_client_registry_dependency*>(binding.client.get());
    if (!dependency) {
      return;
    }
    std::vector<const void*> traversal;
    if (dependency->references_registry(this, traversal)) {
      throw std::invalid_argument(
        "LLM client binding would create a registry ownership cycle: " + binding.provider);
    }
  }

  mutable std::shared_mutex mutex_;
  std::map<std::string, llm_client_binding, std::less<>> bindings_;
};

} // namespace wuwe::agent::llm

#endif // WUWE_AGENT_LLM_LLM_CLIENT_REGISTRY_HPP
