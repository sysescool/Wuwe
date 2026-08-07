#include <wuwe/agent/tools/tool_contract.hpp>

bool tool_contract_header_is_independent() {
  wuwe::agent::tools::tool_descriptor descriptor {
    .name = "header_probe",
  };
  return descriptor.model_tool().name == "header_probe";
}
