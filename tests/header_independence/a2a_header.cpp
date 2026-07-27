#include <wuwe/agent/a2a/a2a.hpp>

#ifdef WUWE_AGENT_MULTI_AGENT_MULTI_AGENT_CORE_HPP
#error "a2a.hpp must not force the optional Multi-Agent adapter"
#endif

bool a2a_header_is_independent() {
  const wuwe::agent::a2a::agent_card card;
  const wuwe::agent::a2a::part part;
  const wuwe::agent::a2a::message message;
  const wuwe::agent::a2a::task task;
  return wuwe::agent::a2a::default_protocol_version == "0.3.0" &&
         card.security_schemes.is_object() && card.security.is_array() &&
         card.metadata.is_object() && part.metadata.is_object() &&
         message.metadata.is_object() && task.metadata.is_object();
}
