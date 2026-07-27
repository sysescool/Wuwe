#include <wuwe/agent/evaluation/security_evaluation.hpp>
#include <wuwe/agent/llm/llm_usage.hpp>
#include <wuwe/agent/llm/resilient_llm_client.hpp>
#include <wuwe/agent/llm/scripted_llm_client.hpp>
#include <wuwe/agent/runtime/run_observability.hpp>

bool p1_header_is_independent() {
  const wuwe::llm_json_schema_output output;
  return output.schema.is_object();
}
