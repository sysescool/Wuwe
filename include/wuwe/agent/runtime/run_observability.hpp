#ifndef WUWE_AGENT_RUNTIME_RUN_OBSERVABILITY_HPP
#define WUWE_AGENT_RUNTIME_RUN_OBSERVABILITY_HPP

#include <cstdint>
#include <string>

#include <wuwe/agent/core/execution_context_projection.hpp>
#include <wuwe/agent/core/observability.hpp>
#include <wuwe/agent/runtime/run_store.hpp>

namespace wuwe::agent::runtime {

[[nodiscard]] inline observability::agent_event to_observability_event(
  const agent_run_event& event, std::string trace_id = {}, std::string request_id = {}) {
  return {
    .module = "runtime",
    .name = event.type,
    .trace_id = std::move(trace_id),
    .subject_id = event.run_id,
    .run_id = event.run_id,
    .sequence = event.sequence,
    .request_id = std::move(request_id),
    .step_id = event.step_id,
    .tool_call_id = event.tool_call_id,
    .timestamp = event.timestamp,
    .attributes = event.metadata,
    .data = event.data,
  };
}

[[nodiscard]] inline observability::agent_event to_observability_event(
  const agent_run_event& event, const core::agent_execution_context& context) {
  auto attributes = event.metadata;
  core::apply_execution_context_attributes(attributes, context);
  return {
    .module = "runtime",
    .name = event.type,
    .trace_id = context.trace_id,
    .subject_id = core::execution_context_subject_id(context),
    .run_id = event.run_id,
    .sequence = event.sequence,
    .request_id = context.request_id,
    .step_id = event.step_id,
    .tool_call_id = event.tool_call_id,
    .timestamp = event.timestamp,
    .attributes = std::move(attributes),
    .data = event.data,
  };
}

inline std::size_t replay_run_events(const agent_run_store& store, const std::string& run_id,
  observability::event_sink& sink, std::uint64_t after_sequence = 0, std::string trace_id = {},
  observability::telemetry_failure_mode failure_mode =
    observability::telemetry_failure_mode::propagate) {
  const auto record = store.load(run_id);
  std::size_t published = 0;
  for (const auto& event : store.list_events(run_id, after_sequence)) {
    if (observability::invoke_telemetry(failure_mode, [&] {
          if (record && trace_id.empty()) {
            sink.publish(to_observability_event(event, record->context));
          }
          else {
            sink.publish(to_observability_event(event, trace_id));
          }
        })) {
      ++published;
    }
  }
  return published;
}

} // namespace wuwe::agent::runtime

#endif // WUWE_AGENT_RUNTIME_RUN_OBSERVABILITY_HPP
