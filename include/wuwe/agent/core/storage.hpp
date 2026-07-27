#ifndef WUWE_AGENT_CORE_STORAGE_HPP
#define WUWE_AGENT_CORE_STORAGE_HPP

#include <cstdint>
#include <optional>
#include <stdexcept>

namespace wuwe::agent::core {

enum class storage_coordination_scope {
  process_local,
  single_node,
  distributed,
};

struct storage_capabilities {
  bool declared { false };
  bool durable { false };
  bool transactional { false };
  bool optimistic_concurrency { false };
  bool atomic_mutations { false };
  bool ordered_replay { false };
  bool schema_migrations { false };
  bool multi_process_safe { false };
  storage_coordination_scope coordination_scope {
    storage_coordination_scope::process_local
  };
  std::optional<std::uint32_t> schema_version;
};

inline void validate_storage_capabilities(
  const storage_capabilities& capabilities) {
  if (!capabilities.declared) {
    return;
  }
  if (capabilities.schema_migrations && !capabilities.schema_version) {
    throw std::invalid_argument(
      "storage declaring migrations must expose a schema version");
  }
  if (capabilities.multi_process_safe &&
      capabilities.coordination_scope ==
        storage_coordination_scope::process_local) {
    throw std::invalid_argument(
      "multi-process storage cannot declare process-local coordination");
  }
  if (capabilities.coordination_scope ==
        storage_coordination_scope::distributed &&
      !capabilities.optimistic_concurrency) {
    throw std::invalid_argument(
      "distributed storage requires optimistic concurrency");
  }
  if (capabilities.transactional && !capabilities.atomic_mutations) {
    throw std::invalid_argument(
      "transactional storage must provide atomic mutations");
  }
}

} // namespace wuwe::agent::core

#endif // WUWE_AGENT_CORE_STORAGE_HPP
