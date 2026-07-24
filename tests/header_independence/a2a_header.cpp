#include <wuwe/agent/a2a/a2a.hpp>

#ifdef WUWE_AGENT_MULTI_AGENT_MULTI_AGENT_CORE_HPP
#error "a2a.hpp must not force the optional Multi-Agent adapter"
#endif

bool a2a_header_is_independent() {
  return wuwe::agent::a2a::default_protocol_version == "0.3.0";
}
