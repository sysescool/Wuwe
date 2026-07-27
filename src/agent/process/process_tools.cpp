#include <wuwe/agent/process/process_tools.hpp>

#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace wuwe::agent::process {
namespace {

nlohmann::json limits_properties(const process_policy& policy) {
  return {
    { "timeout_ms", { { "type", "integer" }, { "minimum", 1 }, { "maximum", policy.max_limits.timeout.count() } } },
    { "max_stdout_bytes", { { "type", "integer" }, { "minimum", 1 }, { "maximum", policy.max_limits.max_stdout_bytes } } },
    { "max_stderr_bytes", { { "type", "integer" }, { "minimum", 1 }, { "maximum", policy.max_limits.max_stderr_bytes } } },
  };
}

nlohmann::json base_properties(const process_policy& policy) {
  auto properties = limits_properties(policy);
  properties["stdin_text"] = {
    { "type", "string" },
    { "maxLength", policy.max_limits.max_stdin_bytes },
  };
  properties["workdir"] = {
    { "type", "string" },
    { "description", "Working-directory path relative to working_directory_root." },
  };
  properties["environment"] = {
    { "type", "object" },
    { "maxProperties", policy.max_limits.max_environment_count },
    { "additionalProperties", {
      { "type", "string" },
      { "maxLength", policy.max_limits.max_environment_bytes },
    } },
  };
  return properties;
}

nlohmann::json result_json(const process_result& result) {
  nlohmann::json output {
    { "termination_reason", to_string(result.termination_reason) },
    { "stdout", result.stdout_text },
    { "stderr", result.stderr_text },
    { "stdout_truncated", result.stdout_truncated },
    { "stderr_truncated", result.stderr_truncated },
    { "elapsed_ms", result.elapsed.count() },
    { "metadata", result.metadata },
  };
  output["exit_code"] = result.exit_code ? nlohmann::json(*result.exit_code) : nlohmann::json(nullptr);
  output["process_id"] = result.process_id ? nlohmann::json(*result.process_id) : nlohmann::json(nullptr);
  if (!result.error_message.empty()) output["error"] = result.error_message;
  return output;
}

std::error_code error_for(process_termination_reason reason) {
  switch (reason) {
    case process_termination_reason::exited: return {};
    case process_termination_reason::timed_out: return std::make_error_code(std::errc::timed_out);
    case process_termination_reason::cancelled: return std::make_error_code(std::errc::operation_canceled);
    case process_termination_reason::policy_denied:
    case process_termination_reason::approval_denied:
      return std::make_error_code(std::errc::permission_denied);
    case process_termination_reason::launch_failed:
      return std::make_error_code(std::errc::no_such_process);
    case process_termination_reason::backend_error:
      return std::make_error_code(std::errc::io_error);
  }
  return std::make_error_code(std::errc::io_error);
}

llm_tool_result as_tool_result(process_result result) {
  return {
    .content = result_json(result).dump(),
    .error_code = error_for(result.termination_reason),
  };
}

std::size_t parsed_size(
  const nlohmann::json& value,
  std::string_view name,
  std::size_t fallback) {
  if (!value.contains(name)) return fallback;
  const auto parsed = value.at(name).get<std::uint64_t>();
  if (parsed > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
    throw std::out_of_range(std::string(name) + " is too large");
  }
  return static_cast<std::size_t>(parsed);
}

process_limits parse_limits(
  const nlohmann::json& args,
  const process_policy& policy) {
  auto limits = policy.max_limits;
  if (args.contains("timeout_ms")) {
    limits.timeout = std::chrono::milliseconds(args.at("timeout_ms").get<std::int64_t>());
  }
  limits.max_stdout_bytes = parsed_size(args, "max_stdout_bytes", limits.max_stdout_bytes);
  limits.max_stderr_bytes = parsed_size(args, "max_stderr_bytes", limits.max_stderr_bytes);
  return limits;
}

} // namespace

process_tool_provider::process_tool_provider(
  process_runtime& runtime,
  process_tool_options options)
    : runtime_(runtime), options_(std::move(options)) {
  if (options_.process_tool_name.empty() || options_.shell_tool_name.empty()) {
    throw std::invalid_argument("process tool names must not be empty");
  }
}

std::vector<llm_tool> process_tool_provider::tools() const {
  std::vector<llm_tool> result;
  const auto& policy = runtime_.policy();
  if (!policy.allowed_executables.empty()) {
    auto properties = base_properties(policy);
    properties["executable"] = {
      { "type", "string" },
      { "minLength", 1 },
      { "description", "Executable from the host-configured allowlist." },
    };
    properties["arguments"] = {
      { "type", "array" },
      { "items", { { "type", "string" } } },
      { "maxItems", policy.max_limits.max_argument_count },
    };
    result.push_back({
      .name = options_.process_tool_name,
      .description = "Run one allowlisted executable with a structured argument vector, bounded input/output, timeout, cancellation, approval, and audit.",
      .parameters_json_schema = nlohmann::json {
        { "type", "object" },
        { "properties", std::move(properties) },
        { "required", { "executable" } },
        { "additionalProperties", false },
      }.dump(),
    });
  }
  if (options_.expose_shell_tool && policy.allow_shell) {
    auto properties = base_properties(policy);
    properties["command"] = {
      { "type", "string" },
      { "minLength", 1 },
      { "maxLength", policy.max_limits.max_argument_bytes },
      { "description", "Raw command interpreted by the configured system shell." },
    };
    result.push_back({
      .name = options_.shell_tool_name,
      .description = "Run a raw shell command. This is a high-risk, explicitly enabled capability and normally requires approval.",
      .parameters_json_schema = nlohmann::json {
        { "type", "object" },
        { "properties", std::move(properties) },
        { "required", { "command" } },
        { "additionalProperties", false },
      }.dump(),
    });
  }
  return result;
}

llm_tool_result process_tool_provider::invoke(
  const std::string& name,
  const std::string& arguments_json) const {
  return invoke(name, arguments_json, {});
}

llm_tool_result process_tool_provider::invoke(
  const std::string& name,
  const std::string& arguments_json,
  std::stop_token stop_token) const {
  if (options_.max_arguments_json_bytes > 0 &&
      arguments_json.size() > options_.max_arguments_json_bytes) {
    runtime_.audit_tool_rejection(name, "tool arguments exceed max_arguments_json_bytes");
    runtime_.audit_tool_rejection(name, "tool not found");
    return {
      .content = "tool arguments are too large",
      .error_code = std::make_error_code(std::errc::value_too_large),
    };
  }
  try {
    const auto args = nlohmann::json::parse(arguments_json);
    if (!args.is_object()) throw std::invalid_argument("tool arguments must be a JSON object");
    const auto environment = args.contains("environment")
      ? args.at("environment").get<std::map<std::string, std::string>>()
      : std::map<std::string, std::string> {};
    if (name == options_.process_tool_name) {
      process_request request {
        .executable = args.at("executable").get<std::string>(),
        .arguments = args.value("arguments", std::vector<std::string> {}),
        .stdin_text = args.value("stdin_text", std::string {}),
        .workdir = args.value("workdir", std::string(".")),
        .environment = environment,
        .limits = parse_limits(args, runtime_.policy()),
      };
      return as_tool_result(runtime_.run(std::move(request), stop_token));
    }
    if (name == options_.shell_tool_name && options_.expose_shell_tool) {
      shell_request request {
        .command = args.at("command").get<std::string>(),
        .stdin_text = args.value("stdin_text", std::string {}),
        .workdir = args.value("workdir", std::string(".")),
        .environment = environment,
        .limits = parse_limits(args, runtime_.policy()),
      };
      return as_tool_result(runtime_.run_shell(std::move(request), stop_token));
    }
    return {
      .content = "tool not found: " + name,
      .error_code = std::make_error_code(std::errc::function_not_supported),
    };
  }
  catch (const std::exception& exception) {
    runtime_.audit_tool_rejection(name, exception.what());
    return {
      .content = std::string("invalid arguments for '") + name + "': " + exception.what(),
      .error_code = std::make_error_code(std::errc::invalid_argument),
    };
  }
  catch (...) {
    runtime_.audit_tool_rejection(name, "unknown tool invocation failure");
    return {
      .content = "tool invocation failed with an unknown exception",
      .error_code = std::make_error_code(std::errc::io_error),
    };
  }
}

} // namespace wuwe::agent::process
