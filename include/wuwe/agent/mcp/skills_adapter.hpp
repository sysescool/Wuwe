#ifndef WUWE_AGENT_MCP_SKILLS_ADAPTER_HPP
#define WUWE_AGENT_MCP_SKILLS_ADAPTER_HPP

#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/capability/capability.hpp>
#include <wuwe/agent/mcp/mcp_host_runtime.hpp>
#include <wuwe/agent/tools/tool_contract.hpp>

namespace wuwe::agent::mcp {

struct mcp_skill_tool_binding {
  std::string server_id;
  std::string tool_name;
  std::string exposed_name;
};

struct mcp_skill_tool_provider_options {
  std::string separator { "__" };
  bool require_running_server { true };
};

// Explicitly binds selected MCP tools into a normal Wuwe tool-provider
// boundary. It never imports every tool from a server implicitly.
class mcp_skill_tool_provider final {
public:
  mcp_skill_tool_provider(mcp_host_runtime& runtime, std::vector<mcp_skill_tool_binding> bindings,
    mcp_skill_tool_provider_options options = {})
      : runtime_(&runtime), options_(std::move(options)) {
    for (auto& binding : bindings) {
      add_binding(std::move(binding));
    }
  }

  [[nodiscard]] std::vector<tools::tool_descriptor> descriptors() const {
    std::vector<tools::tool_descriptor> output;
    output.reserve(bindings_.size());
    for (const auto& [_, binding] : bindings_) {
      output.push_back(binding.descriptor);
    }
    return output;
  }

  [[nodiscard]] std::vector<llm_tool> tools() const {
    std::vector<llm_tool> output;
    output.reserve(bindings_.size());
    for (const auto& [_, binding] : bindings_) {
      output.push_back(binding.descriptor.model_tool());
    }
    return output;
  }

  [[nodiscard]] tools::tool_provider_capabilities contract_capabilities(
    std::string_view) const noexcept {
    // mcp_host_runtime has a synchronous call boundary and cannot guarantee
    // invocation-context, heartbeat, or cooperative cancellation propagation.
    return {};
  }

  llm_tool_result invoke(const std::string& name, const std::string& arguments_json) const {
    return invoke({
      .name = name,
      .arguments_json = arguments_json,
      .descriptor = descriptor_for(name),
    });
  }

  llm_tool_result invoke(const tools::tool_invocation& invocation) const {
    const auto found = bindings_.find(invocation.name);
    if (found == bindings_.end()) {
      return failure("MCP skill tool is not bound: " + invocation.name,
        std::errc::function_not_supported,
        tools::tool_error_category::not_found);
    }
    if (invocation.stop_token.stop_requested()) {
      return failure("MCP skill tool call cancelled before dispatch",
        std::errc::operation_canceled,
        tools::tool_error_category::cancelled);
    }
    try {
      const auto arguments =
        nlohmann::json::parse(invocation.arguments_json.empty() ? "{}" : invocation.arguments_json);
      const auto response =
        runtime_->call_tool(found->second.server_id, found->second.tool_name, arguments);
      return result_from_response(response);
    }
    catch (const nlohmann::json::exception& exception) {
      return failure(std::string("invalid MCP skill tool arguments: ") + exception.what(),
        std::errc::invalid_argument,
        tools::tool_error_category::invalid_input);
    }
    catch (const std::exception& exception) {
      return failure(std::string("MCP skill tool call failed: ") + exception.what(),
        std::errc::host_unreachable,
        tools::tool_error_category::unavailable);
    }
  }

private:
  struct binding_entry {
    std::string server_id;
    std::string tool_name;
    tools::tool_descriptor descriptor;
  };

  void add_binding(mcp_skill_tool_binding binding) {
    if (binding.server_id.empty() || binding.tool_name.empty()) {
      throw std::invalid_argument("MCP skill binding requires server and tool names");
    }
    if (options_.require_running_server && !runtime_->snapshot(binding.server_id).running) {
      throw std::invalid_argument("MCP skill server is not running: " + binding.server_id);
    }
    if (binding.exposed_name.empty()) {
      binding.exposed_name = binding.server_id + options_.separator + binding.tool_name;
    }

    const auto response = runtime_->list_tools(binding.server_id);
    const auto result = response.value("result", nlohmann::json::object());
    const auto listed = result.value("tools", nlohmann::json::array());
    const nlohmann::json* selected = nullptr;
    for (const auto& item : listed) {
      if (item.is_object() && item.value("name", std::string {}) == binding.tool_name) {
        selected = &item;
        break;
      }
    }
    if (!selected) {
      throw std::invalid_argument(
        "MCP skill tool is not advertised by the server: " + binding.tool_name);
    }

    tools::tool_descriptor descriptor {
      .name = binding.exposed_name,
      .version = "mcp",
      .description = selected->value("description", std::string {}),
      .input_schema = selected->value("inputSchema", nlohmann::json::object()),
      .output_schema = selected->value("outputSchema", nlohmann::json::object()),
      .side_effect = tools::tool_side_effect::write,
      .idempotency = tools::tool_idempotency::unknown,
      .approval = tools::tool_approval_mode::policy,
      .capabilities = {
        {
          .name = "mcp.tool.call",
          .risk = capability::capability_risk_level::high,
          .summary = "Invoke an explicitly bound MCP server tool",
          .resources = { binding.server_id + ":" + binding.tool_name },
        },
      },
    };
    tools::validate_tool_descriptor(descriptor);
    if (!bindings_
           .emplace(binding.exposed_name,
             binding_entry {
               .server_id = std::move(binding.server_id),
               .tool_name = std::move(binding.tool_name),
               .descriptor = std::move(descriptor),
             })
           .second) {
      throw std::invalid_argument("duplicate MCP skill tool binding: " + binding.exposed_name);
    }
  }

  [[nodiscard]] tools::tool_descriptor descriptor_for(const std::string& name) const {
    const auto found = bindings_.find(name);
    return found == bindings_.end() ? tools::tool_descriptor { .name = name }
                                    : found->second.descriptor;
  }

  static llm_tool_result result_from_response(const nlohmann::json& response) {
    const auto result = response.value("result", nlohmann::json::object());
    std::string content;
    for (const auto& item : result.value("content", nlohmann::json::array())) {
      if (!content.empty()) {
        content.push_back('\n');
      }
      if (item.is_object() && item.value("type", std::string {}) == "text") {
        content += item.value("text", std::string {});
      }
      else {
        content += item.dump();
      }
    }
    if (result.value("isError", false)) {
      return {
        .content = std::move(content),
        .error_code = std::make_error_code(std::errc::io_error),
        .error_category = tools::tool_error_category::internal,
      };
    }
    return { .content = std::move(content) };
  }

  static llm_tool_result failure(
    std::string message, std::errc code, tools::tool_error_category category) {
    return {
      .content = std::move(message),
      .error_code = std::make_error_code(code),
      .error_category = category,
    };
  }

  mcp_host_runtime* runtime_;
  mcp_skill_tool_provider_options options_;
  std::map<std::string, binding_entry> bindings_;
};

} // namespace wuwe::agent::mcp

#endif // WUWE_AGENT_MCP_SKILLS_ADAPTER_HPP
