#include <wuwe/agent/host/host.hpp>

bool host_header_is_independent() {
  const wuwe::agent::host::host_request_envelope request;
  const wuwe::agent::host::host_response_envelope response;
  const wuwe::agent::host::create_run_request create;
  return wuwe::agent::host::supports_protocol_version(
           wuwe::agent::host::default_protocol_version) &&
         request.body.is_object() && response.body.is_object() && create.input.is_object();
}
