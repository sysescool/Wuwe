#include <wuwe/agent/filesystem/filesystem_tools.hpp>

#include <exception>
#include <limits>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace wuwe::agent::filesystem {
namespace {

nlohmann::json path_property(std::string description) {
  return {
    { "type", "string" },
    { "minLength", 1 },
    { "description", std::move(description) },
  };
}

nlohmann::json object_schema(nlohmann::json properties, nlohmann::json required) {
  return {
    { "type", "object" },
    { "properties", std::move(properties) },
    { "required", std::move(required) },
    { "additionalProperties", false },
  };
}

llm_tool make_tool(std::string name, std::string description, nlohmann::json schema) {
  return {
    .name = std::move(name),
    .description = std::move(description),
    .parameters_json_schema = schema.dump(),
  };
}

nlohmann::json result_json(const filesystem_result& result) {
  nlohmann::json output {
    { "status", to_string(result.status) },
    { "path", result.path.generic_string() },
    { "destination", result.destination.generic_string() },
    { "revision", result.revision },
    { "bytes_processed", result.bytes_processed },
    { "affected_items", result.affected_items },
    { "truncated", result.truncated },
    { "metadata", result.metadata },
  };
  if (!result.error_message.empty())
    output["error"] = result.error_message;
  if (!result.content.empty() || result.successful())
    output["content"] = result.content;
  output["entries"] = nlohmann::json::array();
  for (const auto& entry : result.entries) {
    output["entries"].push_back({
      { "path", entry.path.generic_string() },
      { "type", to_string(entry.type) },
      { "size", entry.size },
      { "revision", entry.revision },
    });
  }
  output["matches"] = nlohmann::json::array();
  for (const auto& match : result.matches) {
    output["matches"].push_back({
      { "path", match.path.generic_string() },
      { "line", match.line },
      { "column", match.column },
      { "text", match.text },
    });
  }
  return output;
}

std::error_code error_for(filesystem_status status) {
  switch (status) {
    case filesystem_status::ok:
      return {};
    case filesystem_status::not_found:
      return std::make_error_code(std::errc::no_such_file_or_directory);
    case filesystem_status::already_exists:
      return std::make_error_code(std::errc::file_exists);
    case filesystem_status::permission_denied:
    case filesystem_status::approval_denied:
    case filesystem_status::outside_root:
      return std::make_error_code(std::errc::permission_denied);
    case filesystem_status::cancelled:
      return std::make_error_code(std::errc::operation_canceled);
    case filesystem_status::invalid_path:
    case filesystem_status::invalid_request:
    case filesystem_status::type_mismatch:
    case filesystem_status::conflict:
    case filesystem_status::limit_exceeded:
      return std::make_error_code(std::errc::invalid_argument);
    case filesystem_status::io_error:
      return std::make_error_code(std::errc::io_error);
  }
  return std::make_error_code(std::errc::io_error);
}

llm_tool_result as_tool_result(filesystem_result result) {
  return {
    .content = result_json(result).dump(),
    .error_code = error_for(result.status),
  };
}

std::size_t bounded_size(const nlohmann::json& value, std::string_view key, std::size_t fallback) {
  if (!value.contains(key))
    return fallback;
  const auto parsed = value.at(key).get<std::uint64_t>();
  if (parsed > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
    throw std::out_of_range(std::string(key) + " is too large");
  }
  return static_cast<std::size_t>(parsed);
}

} // namespace

filesystem_tool_provider::filesystem_tool_provider(
  filesystem_runtime& runtime, filesystem_tool_options options)
    : runtime_(runtime), options_(std::move(options)) {
}

std::string filesystem_tool_provider::tool_name(std::string_view base) const {
  return options_.name_prefix + std::string(base);
}

std::vector<llm_tool> filesystem_tool_provider::tools() const {
  std::vector<llm_tool> result;
  const auto& policy = runtime_.policy();
  if (policy.allow_read) {
    result.push_back(make_tool(tool_name("file_info"),
      "Inspect a path and return its type, size, and an optional content revision.",
      object_schema(
        {
          { "path", path_property("Root-relative path.") },
          { "include_revision", { { "type", "boolean" }, { "default", false } } },
        },
        { "path" })));
    result.push_back(make_tool(tool_name("read_file"),
      "Read one UTF-8 text file inside the configured filesystem root.",
      object_schema(
        {
          { "path", path_property("Root-relative file path.") },
          { "max_bytes",
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", policy.max_read_bytes } } },
        },
        { "path" })));
    result.push_back(make_tool(tool_name("list_directory"),
      "List a directory with explicit recursion and bounded results.",
      object_schema(
        {
          { "path", path_property("Root-relative directory path.") },
          { "recursive", { { "type", "boolean" }, { "default", false } } },
          { "max_depth",
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", policy.max_search_depth } } },
          { "max_entries",
            { { "type", "integer" },
              { "minimum", 1 },
              { "maximum", policy.max_directory_entries } } },
        },
        { "path" })));
    result.push_back(make_tool(tool_name("glob_files"),
      "Find root-relative paths using a bounded glob pattern; ** spans directories.",
      object_schema(
        {
          { "path", path_property("Root-relative directory to search.") },
          { "pattern",
            { { "type", "string" },
              { "minLength", 1 },
              { "maxLength", policy.max_pattern_bytes } } },
          { "max_depth",
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", policy.max_search_depth } } },
          { "max_entries",
            { { "type", "integer" },
              { "minimum", 1 },
              { "maximum", policy.max_directory_entries } } },
        },
        { "path", "pattern" })));
    result.push_back(make_tool(tool_name("search_text"),
      "Search UTF-8 text files for a literal string with bounded traversal and output.",
      object_schema(
        {
          { "path", path_property("Root-relative directory to search.") },
          { "query",
            { { "type", "string" },
              { "minLength", 1 },
              { "maxLength", policy.max_pattern_bytes } } },
          { "file_pattern",
            { { "type", "string" },
              { "default", "**" },
              { "maxLength", policy.max_pattern_bytes } } },
          { "case_sensitive", { { "type", "boolean" }, { "default", true } } },
          { "max_files",
            { { "type", "integer" },
              { "minimum", 1 },
              { "maximum", policy.max_directory_entries } } },
          { "max_results",
            { { "type", "integer" }, { "minimum", 1 }, { "maximum", policy.max_search_results } } },
          { "max_output_bytes",
            { { "type", "integer" },
              { "minimum", 1 },
              { "maximum", policy.max_search_output_bytes } } },
        },
        { "path", "query" })));
  }
  if (policy.allow_write) {
    result.push_back(make_tool(tool_name("write_file"),
      "Atomically create or replace one UTF-8 text file. expected_revision prevents stale "
      "overwrites.",
      object_schema(
        {
          { "path", path_property("Root-relative file path.") },
          { "content", { { "type", "string" }, { "maxLength", policy.max_write_bytes } } },
          { "create_new", { { "type", "boolean" }, { "default", false } } },
          { "expected_revision", { { "type", "string" } } },
          { "create_parent_directories", { { "type", "boolean" }, { "default", false } } },
        },
        { "path", "content" })));
    result.push_back(make_tool(tool_name("replace_text"),
      "Replace an exact text occurrence with revision and occurrence-count protection.",
      object_schema(
        {
          { "path", path_property("Root-relative file path.") },
          { "old_text", { { "type", "string" }, { "minLength", 1 } } },
          { "new_text", { { "type", "string" } } },
          { "expected_revision", { { "type", "string" } } },
          { "expected_replacements",
            { { "type", "integer" }, { "minimum", 1 }, { "default", 1 } } },
          { "replace_all", { { "type", "boolean" }, { "default", false } } },
        },
        { "path", "old_text", "new_text" })));
  }
  if (policy.allow_create_directory)
    result.push_back(make_tool(tool_name("create_directory"),
      "Create a directory inside the configured root.",
      object_schema(
        {
          { "path", path_property("Root-relative directory path.") },
          { "recursive", { { "type", "boolean" }, { "default", true } } },
        },
        { "path" })));
  const auto transfer_schema = [&] {
    return object_schema(
      {
        { "source", path_property("Root-relative source path.") },
        { "destination", path_property("Root-relative destination path.") },
        { "overwrite", { { "type", "boolean" }, { "default", false } } },
        { "recursive", { { "type", "boolean" }, { "default", false } } },
      },
      { "source", "destination" });
  };
  if (policy.allow_copy)
    result.push_back(make_tool(tool_name("copy_path"),
      "Copy a file or an explicitly recursive directory inside the configured root.",
      transfer_schema()));
  if (policy.allow_move)
    result.push_back(make_tool(tool_name("move_path"),
      "Move a file or directory inside the configured root.",
      transfer_schema()));
  if (policy.allow_remove)
    result.push_back(make_tool(tool_name("remove_path"),
      "Remove a file or directory. Recursive deletion must be requested explicitly.",
      object_schema(
        {
          { "path", path_property("Root-relative path to remove.") },
          { "recursive", { { "type", "boolean" }, { "default", false } } },
          { "expected_revision", { { "type", "string" } } },
        },
        { "path" })));
  return result;
}

llm_tool_result filesystem_tool_provider::invoke(
  const std::string& name, const std::string& arguments_json) const {
  return invoke(name, arguments_json, {});
}

llm_tool_result filesystem_tool_provider::invoke(
  const std::string& name, const std::string& arguments_json, std::stop_token stop_token) const {
  if (options_.max_arguments_bytes > 0 && arguments_json.size() > options_.max_arguments_bytes) {
    runtime_.audit_tool_rejection(name, "tool arguments exceed max_arguments_bytes");
    return { .content = "tool arguments are too large",
      .error_code = std::make_error_code(std::errc::value_too_large) };
  }
  try {
    const auto args = nlohmann::json::parse(arguments_json);
    if (!args.is_object())
      throw std::invalid_argument("tool arguments must be a JSON object");
    if (name == tool_name("read_file"))
      return as_tool_result(runtime_.read_text(
        {
          .path = args.at("path").get<std::string>(),
          .max_bytes = bounded_size(args, "max_bytes", 0),
        },
        stop_token));
    if (name == tool_name("file_info"))
      return as_tool_result(runtime_.file_info(
        {
          .path = args.at("path").get<std::string>(),
          .include_revision = args.value("include_revision", false),
        },
        stop_token));
    if (name == tool_name("write_file")) {
      write_text_request request {
        .path = args.at("path").get<std::string>(),
        .content = args.at("content").get<std::string>(),
        .disposition = args.value("create_new", false) ? write_disposition::create_new
                                                       : write_disposition::overwrite,
        .create_parent_directories = args.value("create_parent_directories", false),
      };
      if (args.contains("expected_revision"))
        request.expected_revision = args.at("expected_revision").get<std::string>();
      return as_tool_result(runtime_.write_text(std::move(request), stop_token));
    }
    if (name == tool_name("replace_text")) {
      replace_text_request request {
        .path = args.at("path").get<std::string>(),
        .old_text = args.at("old_text").get<std::string>(),
        .new_text = args.at("new_text").get<std::string>(),
        .expected_replacements = bounded_size(args, "expected_replacements", 1),
        .replace_all = args.value("replace_all", false),
      };
      if (args.contains("expected_revision"))
        request.expected_revision = args.at("expected_revision").get<std::string>();
      return as_tool_result(runtime_.replace_text(std::move(request), stop_token));
    }
    if (name == tool_name("list_directory"))
      return as_tool_result(runtime_.list_directory(
        {
          .path = args.at("path").get<std::string>(),
          .recursive = args.value("recursive", false),
          .max_depth = bounded_size(args, "max_depth", 1),
          .max_entries = bounded_size(args, "max_entries", 1000),
        },
        stop_token));
    if (name == tool_name("glob_files"))
      return as_tool_result(runtime_.glob(
        {
          .path = args.at("path").get<std::string>(),
          .pattern = args.at("pattern").get<std::string>(),
          .max_depth = bounded_size(args, "max_depth", 32),
          .max_entries = bounded_size(args, "max_entries", 1000),
        },
        stop_token));
    if (name == tool_name("search_text"))
      return as_tool_result(runtime_.search_text(
        {
          .path = args.at("path").get<std::string>(),
          .query = args.at("query").get<std::string>(),
          .file_pattern = args.value("file_pattern", std::string("**")),
          .case_sensitive = args.value("case_sensitive", true),
          .max_files = bounded_size(args, "max_files", 10000),
          .max_results = bounded_size(args, "max_results", 200),
          .max_output_bytes = bounded_size(args, "max_output_bytes", 0),
        },
        stop_token));
    if (name == tool_name("create_directory"))
      return as_tool_result(runtime_.create_directory(
        {
          .path = args.at("path").get<std::string>(),
          .recursive = args.value("recursive", true),
        },
        stop_token));
    if (name == tool_name("copy_path") || name == tool_name("move_path")) {
      transfer_path_request request {
        .source = args.at("source").get<std::string>(),
        .destination = args.at("destination").get<std::string>(),
        .overwrite = args.value("overwrite", false),
        .recursive = args.value("recursive", false),
      };
      return as_tool_result(name == tool_name("copy_path")
                              ? runtime_.copy_path(std::move(request), stop_token)
                              : runtime_.move_path(std::move(request), stop_token));
    }
    if (name == tool_name("remove_path")) {
      remove_path_request request {
        .path = args.at("path").get<std::string>(),
        .recursive = args.value("recursive", false),
      };
      if (args.contains("expected_revision"))
        request.expected_revision = args.at("expected_revision").get<std::string>();
      return as_tool_result(runtime_.remove_path(std::move(request), stop_token));
    }
    runtime_.audit_tool_rejection(name, "tool not found");
    return { .content = "tool not found: " + name,
      .error_code = std::make_error_code(std::errc::function_not_supported) };
  }
  catch (const std::exception& error) {
    runtime_.audit_tool_rejection(name, error.what());
    return { .content = std::string("invalid arguments for '") + name + "': " + error.what(),
      .error_code = std::make_error_code(std::errc::invalid_argument) };
  }
  catch (...) {
    runtime_.audit_tool_rejection(name, "unknown tool invocation failure");
    return { .content = "tool invocation failed with an unknown exception",
      .error_code = std::make_error_code(std::errc::io_error) };
  }
}

} // namespace wuwe::agent::filesystem
