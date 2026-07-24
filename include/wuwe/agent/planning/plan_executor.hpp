#ifndef WUWE_AGENT_PLANNING_PLAN_EXECUTOR_HPP
#define WUWE_AGENT_PLANNING_PLAN_EXECUTOR_HPP

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <wuwe/agent/planning/plan.hpp>
#include <wuwe/agent/tools/tool.hpp>

namespace wuwe::agent::planning {

struct plan_executor_capabilities {
  bool cooperative_cancellation { false };
  bool concurrent_execution { true };
};

struct plan_execution_context {
  const plan& current_plan;
  const std::map<std::string, nlohmann::json>& artifacts;
  std::stop_token stop_token;
  std::optional<std::chrono::steady_clock::time_point> deadline;

  [[nodiscard]] bool cancellation_requested() const noexcept {
    return stop_token.stop_requested();
  }

  [[nodiscard]] bool deadline_reached() const noexcept {
    return deadline && std::chrono::steady_clock::now() >= *deadline;
  }

  [[nodiscard]] std::chrono::milliseconds remaining_time() const noexcept {
    if (!deadline) {
      return std::chrono::milliseconds::max();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= *deadline) {
      return std::chrono::milliseconds { 0 };
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
  }
};

class plan_executor {
public:
  virtual ~plan_executor() = default;

  virtual plan_step_result execute(
    const plan_step& step,
    const plan_execution_context& context) = 0;

  [[nodiscard]] virtual plan_executor_capabilities capabilities(
    const plan_step&) const noexcept {
    return {};
  }
};

class function_plan_executor final : public plan_executor {
public:
  using callback = std::function<plan_step_result(const plan_step&, const plan_execution_context&)>;

  explicit function_plan_executor(
    callback execute,
    plan_executor_capabilities capabilities = {})
      : execute_(std::move(execute)), capabilities_(capabilities) {
    if (!execute_) {
      throw std::invalid_argument("function_plan_executor requires a callback");
    }
  }

  plan_step_result execute(
    const plan_step& step,
    const plan_execution_context& context) override {
    return execute_(step, context);
  }

  [[nodiscard]] plan_executor_capabilities capabilities(
    const plan_step&) const noexcept override {
    return capabilities_;
  }

private:
  callback execute_;
  plan_executor_capabilities capabilities_;
};

class tool_plan_executor final : public plan_executor {
public:
  using tools_callback = std::function<std::vector<llm_tool>()>;
  using invoke_callback =
    std::function<llm_tool_result(const std::string&, const std::string&, std::stop_token)>;
  using simple_invoke_callback =
    std::function<llm_tool_result(const std::string&, const std::string&)>;

  tool_plan_executor(
    tools_callback tools,
    invoke_callback invoke,
    plan_executor_capabilities capabilities = {
      .cooperative_cancellation = true,
      })
      : tools_(std::move(tools)), invoke_(std::move(invoke)), capabilities_(capabilities) {
    validate_callbacks();
  }

  tool_plan_executor(
    tools_callback tools,
    simple_invoke_callback invoke,
    plan_executor_capabilities capabilities = {})
      : tools_(std::move(tools)),
        capabilities_(capabilities) {
    if (!tools_ || !invoke) {
      throw std::invalid_argument(
        "tool_plan_executor requires tools and invoke callbacks");
    }
    invoke_ = [invoke = std::move(invoke)](
                const std::string& name,
                const std::string& arguments_json,
                std::stop_token) {
      return invoke(name, arguments_json);
    };
  }

  template<typename ToolProvider>
  explicit tool_plan_executor(std::shared_ptr<ToolProvider> provider)
      : tool_plan_executor(
          std::move(provider),
          detected_provider_capabilities<ToolProvider>()) {
  }

  template<typename ToolProvider>
  tool_plan_executor(
    std::shared_ptr<ToolProvider> provider,
    plan_executor_capabilities capabilities)
      : tools_([provider] { return provider->tools(); }),
        invoke_([provider](
                  const std::string& name,
                  const std::string& arguments_json,
                  std::stop_token stop_token) {
          if constexpr (requires {
                          provider->invoke(name, arguments_json, stop_token);
                        }) {
            return provider->invoke(name, arguments_json, stop_token);
          }
          else {
            return provider->invoke(name, arguments_json);
          }
        }),
        capabilities_(capabilities) {
    if (!provider) {
      throw std::invalid_argument("tool_plan_executor requires a tool provider");
    }
  }

  plan_step_result execute(
    const plan_step& step,
    const plan_execution_context& context) override {
    if (!step.assigned_tool || step.assigned_tool->empty()) {
      return plan_step_result::blocked("step has no assigned tool");
    }

    bool found = false;
    for (const auto& tool : tools_()) {
      if (tool.name == *step.assigned_tool) {
        found = true;
        break;
      }
    }
    if (!found) {
      return plan_step_result::blocked("tool not found: " + *step.assigned_tool);
    }

    const auto arguments = !step.input.empty()
                             ? step.input
                             : (step.input_json.is_object() ? step.input_json.dump() : std::string("{}"));
    const auto result = invoke_(*step.assigned_tool, arguments, context.stop_token);
    if (result.error_code) {
      return plan_step_result {
        .status = plan_step_status::failed,
        .output = result.content,
        .error = result.error_code.message(),
      };
    }

    plan_step_result output = plan_step_result::completed(result.content);
    if (!result.content.empty()) {
      try {
        output.output_json = nlohmann::json::parse(result.content);
      }
      catch (...) {
      }
    }
    return output;
  }

  [[nodiscard]] plan_executor_capabilities capabilities(
    const plan_step&) const noexcept override {
    return capabilities_;
  }

private:
  void validate_callbacks() const {
    if (!tools_ || !invoke_) {
      throw std::invalid_argument(
        "tool_plan_executor requires tools and invoke callbacks");
    }
  }

  template<typename ToolProvider>
  static constexpr plan_executor_capabilities detected_provider_capabilities() {
    return {
      .cooperative_cancellation = requires(
        ToolProvider& value,
        const std::string& name,
        const std::string& arguments,
        std::stop_token stop_token) {
          value.invoke(name, arguments, stop_token);
        },
    };
  }

  tools_callback tools_;
  invoke_callback invoke_;
  plan_executor_capabilities capabilities_;
};

class agent_plan_executor final : public plan_executor {
public:
  using agent_callback =
    std::function<plan_step_result(const plan_step&, const plan_execution_context&)>;

  agent_plan_executor& add_agent(
    std::string name,
    agent_callback callback,
    plan_executor_capabilities capabilities = {}) {
    if (name.empty()) {
      throw std::invalid_argument("agent_plan_executor requires a non-empty agent name");
    }
    if (!callback) {
      throw std::invalid_argument("agent_plan_executor requires an agent callback");
    }
    if (agents_.contains(name)) {
      throw std::invalid_argument("duplicate plan agent executor: " + name);
    }
    agents_[std::move(name)] = {
      .callback = std::move(callback),
      .capabilities = capabilities,
    };
    return *this;
  }

  plan_step_result execute(
    const plan_step& step,
    const plan_execution_context& context) override {
    if (!step.assigned_agent || step.assigned_agent->empty()) {
      return plan_step_result::blocked("step has no assigned agent");
    }

    const auto found = agents_.find(*step.assigned_agent);
    if (found == agents_.end()) {
      return plan_step_result::blocked("agent not found: " + *step.assigned_agent);
    }

    return found->second.callback(step, context);
  }

  [[nodiscard]] plan_executor_capabilities capabilities(
    const plan_step& step) const noexcept override {
    if (!step.assigned_agent || step.assigned_agent->empty()) {
      return {};
    }
    const auto found = agents_.find(*step.assigned_agent);
    return found == agents_.end() ? plan_executor_capabilities {} : found->second.capabilities;
  }

private:
  struct agent_entry {
    agent_callback callback;
    plan_executor_capabilities capabilities;
  };

  std::map<std::string, agent_entry> agents_;
};

class composite_plan_executor final : public plan_executor {
public:
  composite_plan_executor(
    std::shared_ptr<plan_executor> tool_executor,
    std::shared_ptr<plan_executor> agent_executor)
      : tool_executor_(std::move(tool_executor)), agent_executor_(std::move(agent_executor)) {
  }

  plan_step_result execute(
    const plan_step& step,
    const plan_execution_context& context) override {
    if (step.assigned_agent && agent_executor_) {
      return agent_executor_->execute(step, context);
    }
    if (step.assigned_tool && tool_executor_) {
      return tool_executor_->execute(step, context);
    }
    return plan_step_result::blocked("step has no assigned executor");
  }

  [[nodiscard]] plan_executor_capabilities capabilities(
    const plan_step& step) const noexcept override {
    if (step.assigned_agent && agent_executor_) {
      return agent_executor_->capabilities(step);
    }
    if (step.assigned_tool && tool_executor_) {
      return tool_executor_->capabilities(step);
    }
    return {};
  }

private:
  std::shared_ptr<plan_executor> tool_executor_;
  std::shared_ptr<plan_executor> agent_executor_;
};

} // namespace wuwe::agent::planning

#endif // WUWE_AGENT_PLANNING_PLAN_EXECUTOR_HPP
