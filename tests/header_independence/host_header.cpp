#include <wuwe/agent/host/host.hpp>

bool host_header_is_independent() {
  return wuwe::agent::host::supports_protocol_version(
    wuwe::agent::host::default_protocol_version);
}
