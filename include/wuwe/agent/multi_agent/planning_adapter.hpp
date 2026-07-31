#ifndef WUWE_AGENT_MULTI_AGENT_PLANNING_ADAPTER_HPP
#define WUWE_AGENT_MULTI_AGENT_PLANNING_ADAPTER_HPP

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <wuwe/agent/multi_agent/team_runtime.hpp>
#include <wuwe/agent/planning/plan_executor.hpp>

namespace wuwe::agent::multi_agent {

class team_plan_executor final : public planning::plan_executor {
public:
  explicit team_plan_executor(std::shared_ptr<team_runtime> runtime)
      : runtime_(std::move(runtime)) {
    if (!runtime_) {
      throw std::invalid_argument("team_plan_executor requires a team runtime");
    }
  }

  planning::plan_step_result execute(
    const planning::plan_step& step, const planning::plan_execution_context& context) override {
    if (!step.assigned_agent || step.assigned_agent->empty()) {
      return planning::plan_step_result::blocked("step has no assigned agent");
    }
    auto input = step.input;
    if (input.empty() && !step.input_json.is_null()) {
      input = step.input_json.dump();
    }
    const auto result = runtime_->run(
      {
        .id = context.current_plan.id + ":" + step.id,
        .session_id = context.current_plan.id,
        .input = std::move(input),
        .preferred_agent = *step.assigned_agent,
        .metadata = step.metadata,
      },
      context.stop_token);

    planning::plan_step_result output;
    output.output = result.output;
    output.error = result.error;
    output.metadata = result.metadata;
    output.metadata["agent_id"] = result.agent_id;
    output.metadata["agent_task_status"] = to_string(result.status);
    try {
      if (!result.output.empty()) {
        output.output_json = nlohmann::json::parse(result.output);
      }
    }
    catch (...) {
    }
    for (const auto& artifact : result.artifacts) {
      output.artifacts[artifact.id] =
        artifact.data.is_null() ? nlohmann::json(artifact.content) : artifact.data;
    }
    switch (result.status) {
      case agent_task_status::completed:
        output.status = planning::plan_step_status::completed;
        break;
      case agent_task_status::blocked:
      case agent_task_status::input_required:
        output.status = planning::plan_step_status::blocked;
        break;
      case agent_task_status::cancelled:
      case agent_task_status::timed_out:
      case agent_task_status::failed:
      case agent_task_status::submitted:
      case agent_task_status::working:
        output.status = planning::plan_step_status::failed;
        break;
    }
    return output;
  }

  [[nodiscard]] planning::plan_executor_capabilities capabilities(
    const planning::plan_step& step) const noexcept override {
    if (!step.assigned_agent || step.assigned_agent->empty()) {
      return {};
    }
    const auto registered = runtime_->registry()->find(*step.assigned_agent);
    if (!registered) {
      return {};
    }
    return {
      .cooperative_cancellation = registered->executor_capabilities.cooperative_cancellation,
      .concurrent_execution = registered->executor_capabilities.concurrent_execution,
    };
  }

private:
  std::shared_ptr<team_runtime> runtime_;
};

} // namespace wuwe::agent::multi_agent

#endif // WUWE_AGENT_MULTI_AGENT_PLANNING_ADAPTER_HPP
