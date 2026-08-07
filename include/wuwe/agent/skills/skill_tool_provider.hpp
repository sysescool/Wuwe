#ifndef WUWE_AGENT_SKILLS_SKILL_TOOL_PROVIDER_HPP
#define WUWE_AGENT_SKILLS_SKILL_TOOL_PROVIDER_HPP

#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <wuwe/agent/tools/tool.hpp>

namespace wuwe::agent::skills {

// A skill activation must restrict both the model-visible tool list and the
// provider invocation boundary. Filtering only llm_request::tools would still
// allow application code to invoke a non-activated tool through the provider.
template<typename Provider>
class scoped_tool_provider {
public:
  scoped_tool_provider(std::shared_ptr<Provider> provider, std::vector<std::string> allowed_tools)
      : provider_(std::move(provider)) {
    if (!provider_) {
      throw std::invalid_argument("scoped tool provider requires a provider");
    }

    std::map<std::string, tools::tool_descriptor> available;
    if constexpr (requires { provider_->descriptors(); }) {
      for (auto descriptor : provider_->descriptors()) {
        tools::validate_tool_descriptor(descriptor);
        if (!available.emplace(descriptor.name, std::move(descriptor)).second) {
          throw std::invalid_argument("tool provider returned a duplicate descriptor");
        }
      }
    }
    else {
      for (const auto& model_tool : provider_->tools()) {
        auto descriptor = tools::descriptor_from_llm_tool(model_tool);
        tools::validate_tool_descriptor(descriptor);
        if (!available.emplace(descriptor.name, std::move(descriptor)).second) {
          throw std::invalid_argument("tool provider returned a duplicate descriptor");
        }
      }
    }

    for (auto& name : allowed_tools) {
      if (name.empty()) {
        throw std::invalid_argument("activated tool names cannot be empty");
      }
      const auto found = available.find(name);
      if (found == available.end()) {
        throw std::invalid_argument("activated tool is not provided: " + name);
      }
      if (!descriptors_.emplace(name, found->second).second) {
        throw std::invalid_argument("duplicate activated tool: " + name);
      }
    }
  }

  [[nodiscard]] std::vector<tools::tool_descriptor> descriptors() const {
    std::vector<tools::tool_descriptor> output;
    output.reserve(descriptors_.size());
    for (const auto& [_, descriptor] : descriptors_) {
      output.push_back(descriptor);
    }
    return output;
  }

  [[nodiscard]] std::vector<llm_tool> tools() const {
    std::vector<llm_tool> output;
    output.reserve(descriptors_.size());
    for (const auto& [_, descriptor] : descriptors_) {
      output.push_back(descriptor.model_tool());
    }
    return output;
  }

  [[nodiscard]] tools::tool_provider_capabilities contract_capabilities(
    std::string_view name) const noexcept {
    if (!contains(name)) {
      return {};
    }
    return tools::resolve_tool_provider_capabilities(*provider_, std::string(name));
  }

  llm_tool_result invoke(const std::string& name, const std::string& arguments_json) const {
    auto descriptor = descriptor_for(name);
    return invoke({
      .name = name,
      .arguments_json = arguments_json,
      .descriptor = descriptor.value_or(tools::tool_descriptor { .name = name }),
    });
  }

  llm_tool_result invoke(const tools::tool_invocation& invocation) const {
    const auto descriptor = descriptor_for(invocation.name);
    if (!descriptor) {
      return denied(invocation.name);
    }
    auto scoped_invocation = invocation;
    scoped_invocation.descriptor = *descriptor;
    if constexpr (requires { provider_->invoke(invocation); }) {
      return provider_->invoke(scoped_invocation);
    }
    else if constexpr (requires {
                         provider_->invoke(
                           invocation.name, invocation.arguments_json, invocation.stop_token);
                       }) {
      return provider_->invoke(
        scoped_invocation.name, scoped_invocation.arguments_json, scoped_invocation.stop_token);
    }
    else {
      return provider_->invoke(scoped_invocation.name, scoped_invocation.arguments_json);
    }
  }

  llm_tool_result compensate(
    const tools::tool_invocation& invocation, const llm_tool_result& outcome) const {
    const auto descriptor = descriptor_for(invocation.name);
    if (!descriptor) {
      return denied(invocation.name);
    }
    auto scoped_invocation = invocation;
    scoped_invocation.descriptor = *descriptor;
    if constexpr (requires { provider_->compensate(invocation, outcome); }) {
      return provider_->compensate(scoped_invocation, outcome);
    }
    return {
      .content = "tool provider does not support compensation",
      .error_code = std::make_error_code(std::errc::function_not_supported),
      .error_category = tools::tool_error_category::internal,
    };
  }

private:
  [[nodiscard]] bool contains(std::string_view name) const noexcept {
    return descriptors_.contains(std::string(name));
  }

  [[nodiscard]] std::optional<tools::tool_descriptor> descriptor_for(
    const std::string& name) const {
    const auto found = descriptors_.find(name);
    return found == descriptors_.end() ? std::nullopt
                                       : std::optional<tools::tool_descriptor>(found->second);
  }

  static llm_tool_result denied(const std::string& name) {
    return {
      .content = "tool is not activated for this skill scope: " + name,
      .error_code = std::make_error_code(std::errc::permission_denied),
      .error_category = tools::tool_error_category::permission_denied,
    };
  }

  std::shared_ptr<Provider> provider_;
  std::map<std::string, tools::tool_descriptor> descriptors_;
};

} // namespace wuwe::agent::skills

#endif // WUWE_AGENT_SKILLS_SKILL_TOOL_PROVIDER_HPP
