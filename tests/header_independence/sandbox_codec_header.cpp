#include <wuwe/agent/sandbox/sandbox_codec.hpp>

bool sandbox_codec_header_is_independent() {
  return wuwe::agent::sandbox::sandbox_policy_to_json({}).value("schema_version", 0) == 1;
}
