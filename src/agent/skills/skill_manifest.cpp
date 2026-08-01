#include <wuwe/agent/skills/skill_manifest.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <stdexcept>

#include <wuwe/agent/tools/json_schema.hpp>

namespace wuwe::agent::skills {
namespace {

using json = nlohmann::json;

[[noreturn]] void invalid(const std::string& path, const std::string& message) {
  throw std::invalid_argument("invalid skill manifest at " + path + ": " + message);
}

void require_object(const json& value, const std::string& path) {
  if (!value.is_object()) {
    invalid(path, "expected an object");
  }
}

void reject_unknown(
  const json& value, std::initializer_list<std::string_view> keys, const std::string& path) {
  require_object(value, path);
  for (const auto& [name, unused] : value.items()) {
    (void)unused;
    if (std::none_of(
          keys.begin(), keys.end(), [&](std::string_view allowed) { return name == allowed; })) {
      invalid(path + "." + name, "unknown field");
    }
  }
}

const json& required(const json& value, const char* key, const std::string& path) {
  if (!value.contains(key)) {
    invalid(path + "." + key, "required field is missing");
  }
  return value.at(key);
}

std::string string_value(const json& value, const std::string& path, bool nonempty = true) {
  if (!value.is_string()) {
    invalid(path, "expected a string");
  }
  auto output = value.get<std::string>();
  if (nonempty && output.empty()) {
    invalid(path, "must not be empty");
  }
  return output;
}

bool bool_value(const json& value, const std::string& path) {
  if (!value.is_boolean()) {
    invalid(path, "expected a boolean");
  }
  return value.get<bool>();
}

std::size_t size_value(const json& value, const std::string& path) {
  if (!value.is_number_unsigned()) {
    invalid(path, "expected a non-negative integer");
  }
  const auto parsed = value.get<std::uint64_t>();
  if (parsed > std::numeric_limits<std::size_t>::max()) {
    invalid(path, "integer is too large for this platform");
  }
  return static_cast<std::size_t>(parsed);
}

std::map<std::string, std::string> string_map(const json& value, const std::string& path) {
  require_object(value, path);
  std::map<std::string, std::string> output;
  for (const auto& [name, item] : value.items()) {
    output.emplace(name, string_value(item, path + "." + name, false));
  }
  return output;
}

std::vector<std::string> string_array(const json& value, const std::string& path) {
  if (!value.is_array()) {
    invalid(path, "expected an array");
  }
  std::vector<std::string> output;
  std::set<std::string> unique;
  for (std::size_t index = 0; index < value.size(); ++index) {
    auto item = string_value(value.at(index), path + "[" + std::to_string(index) + "]");
    if (!unique.insert(item).second) {
      invalid(path, "contains duplicate value '" + item + "'");
    }
    output.push_back(std::move(item));
  }
  return output;
}

void validate_json_limits(const json& value, const skill_manifest_limits& limits, std::size_t depth,
  const std::string& path) {
  if (depth > limits.max_json_depth) {
    invalid(path, "JSON nesting exceeds the configured limit");
  }
  if (value.is_string() && value.get_ref<const std::string&>().size() > limits.max_string_bytes) {
    invalid(path, "string exceeds the configured limit");
  }
  if ((value.is_array() || value.is_object()) && value.size() > limits.max_collection_items) {
    invalid(path, "collection exceeds the configured limit");
  }
  if (value.is_array()) {
    for (std::size_t index = 0; index < value.size(); ++index) {
      validate_json_limits(
        value.at(index), limits, depth + 1, path + "[" + std::to_string(index) + "]");
    }
  }
  else if (value.is_object()) {
    for (const auto& [name, item] : value.items()) {
      if (name.size() > limits.max_string_bytes) {
        invalid(path, "field name exceeds the configured limit");
      }
      validate_json_limits(item, limits, depth + 1, path + "." + name);
    }
  }
}

bool valid_id(std::string_view value) {
  if (value.empty() || value.size() > 255 || value.front() == '.' || value.back() == '.') {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char item) {
    const auto byte = static_cast<unsigned char>(item);
    return std::isalnum(byte) != 0 || item == '.' || item == '_' || item == '-';
  });
}

bool valid_resource_path(std::string_view value) {
  constexpr std::size_t maximum_path_bytes = 1024;
  constexpr std::size_t maximum_component_bytes = 255;
  constexpr std::size_t maximum_components = 32;
  if (value.empty() || value.size() > maximum_path_bytes || value.front() == '/' ||
      value.front() == '\\' || value.find(':') != std::string_view::npos ||
      value.find('\\') != std::string_view::npos ||
      std::any_of(value.begin(), value.end(), [](char item) {
        const auto byte = static_cast<unsigned char>(item);
        return byte < 0x20 || byte == 0x7f;
      })) {
    return false;
  }
  std::size_t offset = 0;
  std::size_t component_count = 0;
  while (offset <= value.size()) {
    const auto end = value.find('/', offset);
    const auto part =
      value.substr(offset, end == std::string_view::npos ? value.size() - offset : end - offset);
    if (part.empty() || part == "." || part == ".." || part.size() > maximum_component_bytes ||
        ++component_count > maximum_components || part.back() == ' ' || part.back() == '.') {
      return false;
    }
    std::string stem(part.substr(0, part.find('.')));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char item) {
      return item >= 'A' && item <= 'Z' ? static_cast<char>(item - 'A' + 'a')
                                        : static_cast<char>(item);
    });
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
        (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
          stem[3] >= '1' && stem[3] <= '9')) {
      return false;
    }
    if (std::any_of(part.begin(), part.end(), [](char raw) {
          const auto item = static_cast<unsigned char>(raw);
          return item == '<' || item == '>' || item == '"' || item == '|' || item == '?' ||
                 item == '*';
        })) {
      return false;
    }
    if (end == std::string_view::npos) {
      break;
    }
    offset = end + 1;
  }
  return true;
}

bool valid_sha256(std::string_view value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char item) {
    return (item >= '0' && item <= '9') || (item >= 'a' && item <= 'f');
  });
}

std::string portable_case_key(std::string_view value) {
  std::string output(value);
  std::transform(output.begin(), output.end(), output.begin(), [](unsigned char item) {
    return item >= 'A' && item <= 'Z' ? static_cast<char>(item - 'A' + 'a')
                                      : static_cast<char>(item);
  });
  return output;
}

void validate_json_schema_contract(
  const json& schema, const std::string& path, const json* root = nullptr) {
  if (!schema.is_object() && !schema.is_boolean()) {
    invalid(path, "JSON Schema must be an object or boolean");
  }
  const bool top_level = root == nullptr;
  root = top_level ? &schema : root;
  if (top_level) {
    const auto probe = tools::json_schema_validator().validate(json::object(), schema);
    for (const auto& issue : probe.issues) {
      if (issue.message.starts_with("unsupported JSON Schema assertion:") ||
          issue.message == "schema must be an object or boolean" ||
          issue.message.starts_with("only valid local JSON Pointer references are supported")) {
        invalid(path + issue.schema_path, issue.message);
      }
    }
  }
  if (schema.is_object()) {
    static constexpr std::string_view unsupported[] { "patternProperties",
      "propertyNames",
      "dependentRequired",
      "dependentSchemas",
      "unevaluatedProperties",
      "prefixItems",
      "contains",
      "minContains",
      "maxContains",
      "unevaluatedItems",
      "if",
      "then",
      "else" };
    for (const auto& [name, value] : schema.items()) {
      if (std::find(std::begin(unsupported), std::end(unsupported), name) !=
          std::end(unsupported)) {
        invalid(path + "/" + name, "unsupported JSON Schema assertion: " + name);
      }
      if (name == "$ref") {
        if (!value.is_string() || !value.get_ref<const std::string&>().starts_with('#')) {
          invalid(path + "/$ref", "only local JSON Pointer references are supported");
        }
        const auto& reference = value.get_ref<const std::string&>();
        try {
          if (reference != "#") {
            if (!reference.starts_with("#/")) {
              invalid(path + "/$ref", "local reference must be a JSON Pointer");
            }
            (void)root->at(json::json_pointer(reference.substr(1)));
          }
        }
        catch (const json::exception&) {
          invalid(path + "/$ref", "local JSON Pointer reference does not resolve");
        }
      }
      if ((name == "properties" || name == "$defs" || name == "definitions") && value.is_object()) {
        for (const auto& [child_name, child] : value.items()) {
          validate_json_schema_contract(child, path + "/" + name + "/" + child_name, root);
        }
      }
      else if ((name == "items" || name == "additionalProperties" || name == "not") &&
               (value.is_object() || value.is_boolean())) {
        validate_json_schema_contract(value, path + "/" + name, root);
      }
      else if ((name == "allOf" || name == "anyOf" || name == "oneOf") && value.is_array()) {
        for (std::size_t index = 0; index < value.size(); ++index) {
          validate_json_schema_contract(
            value.at(index), path + "/" + name + "/" + std::to_string(index), root);
        }
      }
    }
  }
}

capability::capability_risk_level risk_from_string(
  const std::string& value, const std::string& path) {
  if (value == "low") {
    return capability::capability_risk_level::low;
  }
  if (value == "medium") {
    return capability::capability_risk_level::medium;
  }
  if (value == "high") {
    return capability::capability_risk_level::high;
  }
  if (value == "critical") {
    return capability::capability_risk_level::critical;
  }
  invalid(path, "unsupported capability risk level");
}

skill_resource_descriptor parse_resource(const json& value, const std::string& path) {
  reject_unknown(value, { "id", "path", "kind", "media_type", "size", "sha256", "metadata" }, path);
  skill_resource_descriptor output;
  output.id = string_value(required(value, "id", path), path + ".id");
  output.path = string_value(required(value, "path", path), path + ".path");
  output.kind =
    skill_resource_kind_from_string(string_value(required(value, "kind", path), path + ".kind"));
  if (value.contains("media_type")) {
    output.media_type = string_value(value.at("media_type"), path + ".media_type");
  }
  output.size = size_value(required(value, "size", path), path + ".size");
  output.sha256 = string_value(required(value, "sha256", path), path + ".sha256");
  if (value.contains("metadata")) {
    output.metadata = string_map(value.at("metadata"), path + ".metadata");
  }
  return output;
}

template<typename Value, typename Parser>
std::vector<Value> parse_array(const json& value, const std::string& path, Parser parser) {
  if (!value.is_array()) {
    invalid(path, "expected an array");
  }
  std::vector<Value> output;
  output.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    output.push_back(parser(value.at(index), path + "[" + std::to_string(index) + "]"));
  }
  return output;
}

} // namespace

std::string to_string(skill_resource_kind value) {
  switch (value) {
    case skill_resource_kind::instructions:
      return "instructions";
    case skill_resource_kind::prompt:
      return "prompt";
    case skill_resource_kind::schema:
      return "schema";
    case skill_resource_kind::knowledge:
      return "knowledge";
    case skill_resource_kind::script:
      return "script";
    case skill_resource_kind::template_:
      return "template";
    case skill_resource_kind::asset:
      return "asset";
  }
  throw std::invalid_argument("unsupported skill resource kind");
}

skill_resource_kind skill_resource_kind_from_string(const std::string& value) {
  if (value == "instructions") {
    return skill_resource_kind::instructions;
  }
  if (value == "prompt") {
    return skill_resource_kind::prompt;
  }
  if (value == "schema") {
    return skill_resource_kind::schema;
  }
  if (value == "knowledge") {
    return skill_resource_kind::knowledge;
  }
  if (value == "script") {
    return skill_resource_kind::script;
  }
  if (value == "template") {
    return skill_resource_kind::template_;
  }
  if (value == "asset") {
    return skill_resource_kind::asset;
  }
  throw std::invalid_argument("unsupported skill resource kind '" + value + "'");
}

std::string to_string(skill_origin_kind value) {
  switch (value) {
    case skill_origin_kind::embedded:
      return "embedded";
    case skill_origin_kind::local:
      return "local";
    case skill_origin_kind::remote:
      return "remote";
  }
  throw std::invalid_argument("unsupported skill origin kind");
}

skill_manifest parse_skill_manifest(std::string_view input, const skill_manifest_limits& limits) {
  if (input.size() > limits.max_manifest_bytes) {
    throw std::invalid_argument("skill manifest exceeds the configured byte limit");
  }
  try {
    std::vector<std::set<std::string>> object_keys;
    const auto reject_duplicates = [&object_keys](
                                     int depth, json::parse_event_t event, json& parsed) {
      (void)depth;
      if (event == json::parse_event_t::object_start) {
        object_keys.emplace_back();
      }
      else if (event == json::parse_event_t::key) {
        const auto& key = parsed.get_ref<const std::string&>();
        if (object_keys.empty() || !object_keys.back().insert(key).second) {
          throw std::invalid_argument(
            "invalid skill manifest JSON: duplicate object key '" + key + "'");
        }
      }
      else if (event == json::parse_event_t::object_end) {
        object_keys.pop_back();
      }
      return true;
    };
    return skill_manifest_from_json(json::parse(input, reject_duplicates), limits);
  }
  catch (const json::exception& error) {
    throw std::invalid_argument(std::string("invalid skill manifest JSON: ") + error.what());
  }
}

skill_manifest skill_manifest_from_json(const json& input, const skill_manifest_limits& limits) {
  validate_json_limits(input, limits, 0, "$");
  reject_unknown(input,
    { "schema_version",
      "id",
      "version",
      "name",
      "description",
      "tags",
      "examples",
      "input_modes",
      "output_modes",
      "input_schema",
      "output_schema",
      "deprecated",
      "instructions",
      "resources",
      "requires",
      "metadata" },
    "$");

  skill_manifest output;
  output.schema_version = size_value(required(input, "schema_version", "$"), "$.schema_version");
  if (output.schema_version != current_skill_manifest_schema_version) {
    invalid("$.schema_version", "unsupported schema version");
  }
  output.descriptor.id = string_value(required(input, "id", "$"), "$.id");
  output.descriptor.version =
    semantic_version::parse(string_value(required(input, "version", "$"), "$.version"));
  output.descriptor.name = string_value(required(input, "name", "$"), "$.name");
  output.descriptor.description =
    string_value(required(input, "description", "$"), "$.description");
  if (input.contains("tags")) {
    output.descriptor.tags = string_array(input.at("tags"), "$.tags");
  }
  if (input.contains("examples")) {
    output.descriptor.examples = string_array(input.at("examples"), "$.examples");
  }
  if (input.contains("input_modes")) {
    output.descriptor.input_modes = string_array(input.at("input_modes"), "$.input_modes");
  }
  if (input.contains("output_modes")) {
    output.descriptor.output_modes = string_array(input.at("output_modes"), "$.output_modes");
  }
  if (input.contains("input_schema")) {
    output.descriptor.input_schema = input.at("input_schema");
  }
  if (input.contains("output_schema")) {
    output.descriptor.output_schema = input.at("output_schema");
  }
  if (input.contains("deprecated")) {
    output.descriptor.deprecated = bool_value(input.at("deprecated"), "$.deprecated");
  }
  if (input.contains("instructions")) {
    output.instruction_resources = string_array(input.at("instructions"), "$.instructions");
  }
  if (input.contains("resources")) {
    output.resources = parse_array<skill_resource_descriptor>(input.at("resources"),
      "$.resources",
      [](const json& value, const std::string& path) { return parse_resource(value, path); });
  }

  if (input.contains("requires")) {
    const auto& requirements = input.at("requires");
    reject_unknown(requirements, { "skills", "tools", "capabilities", "knowledge" }, "$.requires");
    if (requirements.contains("skills")) {
      output.dependencies = parse_array<skill_dependency>(requirements.at("skills"),
        "$.requires.skills",
        [](const json& value, const std::string& path) {
          reject_unknown(value, { "id", "version", "optional" }, path);
          skill_dependency dependency;
          dependency.id = string_value(required(value, "id", path), path + ".id");
          dependency.version = version_requirement::parse(
            string_value(required(value, "version", path), path + ".version"));
          if (value.contains("optional")) {
            dependency.optional = bool_value(value.at("optional"), path + ".optional");
          }
          return dependency;
        });
    }
    if (requirements.contains("tools")) {
      output.tools = parse_array<skill_tool_requirement>(requirements.at("tools"),
        "$.requires.tools",
        [](const json& value, const std::string& path) {
          reject_unknown(value, { "name", "exact_version", "optional" }, path);
          skill_tool_requirement tool;
          tool.name = string_value(required(value, "name", path), path + ".name");
          if (value.contains("exact_version")) {
            tool.exact_version = string_value(value.at("exact_version"), path + ".exact_version");
          }
          if (value.contains("optional")) {
            tool.optional = bool_value(value.at("optional"), path + ".optional");
          }
          return tool;
        });
    }
    if (requirements.contains("capabilities")) {
      output.capabilities =
        parse_array<skill_capability_requirement>(requirements.at("capabilities"),
          "$.requires.capabilities",
          [](const json& value, const std::string& path) {
            reject_unknown(value, { "name", "risk", "summary", "resources", "metadata" }, path);
            skill_capability_requirement capability;
            capability.name = string_value(required(value, "name", path), path + ".name");
            capability.risk = risk_from_string(
              string_value(required(value, "risk", path), path + ".risk"), path + ".risk");
            capability.summary = string_value(required(value, "summary", path), path + ".summary");
            if (value.contains("resources")) {
              capability.resources = string_array(value.at("resources"), path + ".resources");
            }
            if (value.contains("metadata")) {
              capability.metadata = string_map(value.at("metadata"), path + ".metadata");
            }
            return capability;
          });
    }
    if (requirements.contains("knowledge")) {
      output.knowledge = parse_array<skill_knowledge_requirement>(requirements.at("knowledge"),
        "$.requires.knowledge",
        [](const json& value, const std::string& path) {
          reject_unknown(value, { "source", "filters", "max_context_chars", "optional" }, path);
          skill_knowledge_requirement knowledge;
          knowledge.source = string_value(required(value, "source", path), path + ".source");
          if (value.contains("filters")) {
            knowledge.filters = string_map(value.at("filters"), path + ".filters");
          }
          if (value.contains("max_context_chars")) {
            knowledge.max_context_chars =
              size_value(value.at("max_context_chars"), path + ".max_context_chars");
          }
          if (value.contains("optional")) {
            knowledge.optional = bool_value(value.at("optional"), path + ".optional");
          }
          return knowledge;
        });
    }
  }
  if (input.contains("metadata")) {
    output.metadata = string_map(input.at("metadata"), "$.metadata");
    output.descriptor.metadata = output.metadata;
  }
  validate_skill_manifest(output);
  return output;
}

json skill_manifest_to_json(const skill_manifest& manifest) {
  validate_skill_manifest(manifest);
  json output {
    { "schema_version", manifest.schema_version },
    { "id", manifest.descriptor.id },
    { "version", manifest.descriptor.version.string() },
    { "name", manifest.descriptor.name },
    { "description", manifest.descriptor.description },
    { "tags", manifest.descriptor.tags },
    { "examples", manifest.descriptor.examples },
    { "input_modes", manifest.descriptor.input_modes },
    { "output_modes", manifest.descriptor.output_modes },
    { "input_schema", manifest.descriptor.input_schema },
    { "output_schema", manifest.descriptor.output_schema },
    { "deprecated", manifest.descriptor.deprecated },
    { "instructions", manifest.instruction_resources },
    { "resources", json::array() },
    { "requires",
      { { "skills", json::array() },
        { "tools", json::array() },
        { "capabilities", json::array() },
        { "knowledge", json::array() } } },
    { "metadata", manifest.metadata },
  };
  for (const auto& resource : manifest.resources) {
    output["resources"].push_back({ { "id", resource.id },
      { "path", resource.path },
      { "kind", to_string(resource.kind) },
      { "media_type", resource.media_type },
      { "size", resource.size },
      { "sha256", resource.sha256 },
      { "metadata", resource.metadata } });
  }
  for (const auto& dependency : manifest.dependencies) {
    output["requires"]["skills"].push_back({ { "id", dependency.id },
      { "version", dependency.version.expression() },
      { "optional", dependency.optional } });
  }
  for (const auto& tool : manifest.tools) {
    json encoded { { "name", tool.name }, { "optional", tool.optional } };
    if (tool.exact_version) {
      encoded["exact_version"] = *tool.exact_version;
    }
    output["requires"]["tools"].push_back(std::move(encoded));
  }
  for (const auto& capability : manifest.capabilities) {
    output["requires"]["capabilities"].push_back({ { "name", capability.name },
      { "risk", capability::to_string(capability.risk) },
      { "summary", capability.summary },
      { "resources", capability.resources },
      { "metadata", capability.metadata } });
  }
  for (const auto& knowledge : manifest.knowledge) {
    output["requires"]["knowledge"].push_back({ { "source", knowledge.source },
      { "filters", knowledge.filters },
      { "max_context_chars", knowledge.max_context_chars },
      { "optional", knowledge.optional } });
  }
  return output;
}

void validate_skill_manifest(const skill_manifest& manifest) {
  if (manifest.schema_version != current_skill_manifest_schema_version) {
    throw std::invalid_argument("unsupported skill manifest schema version");
  }
  if (!valid_id(manifest.descriptor.id)) {
    throw std::invalid_argument("skill id must be a stable dot-separated identifier");
  }
  if (manifest.descriptor.name.empty() || manifest.descriptor.description.empty()) {
    throw std::invalid_argument("skill name and description must not be empty");
  }
  validate_json_schema_contract(manifest.descriptor.input_schema, "$.input_schema");
  validate_json_schema_contract(manifest.descriptor.output_schema, "$.output_schema");

  std::set<std::string> resource_ids;
  std::set<std::string> resource_paths;
  for (const auto& resource : manifest.resources) {
    if (!valid_id(resource.id)) {
      throw std::invalid_argument("skill resource has an invalid id");
    }
    if (!valid_resource_path(resource.path)) {
      throw std::invalid_argument("skill resource path must be normalized and relative");
    }
    if (!valid_sha256(resource.sha256)) {
      throw std::invalid_argument(
        "skill resource sha256 must contain 64 lowercase hexadecimal characters");
    }
    if (!resource_ids.insert(resource.id).second ||
        !resource_paths.insert(portable_case_key(resource.path)).second) {
      throw std::invalid_argument("skill resource ids and paths must be unique");
    }
  }
  std::set<std::string> instruction_ids;
  for (const auto& instruction : manifest.instruction_resources) {
    if (!instruction_ids.insert(instruction).second) {
      throw std::invalid_argument("skill instruction resources must be unique");
    }
    if (!resource_ids.contains(instruction)) {
      throw std::invalid_argument("instruction resource '" + instruction + "' is not declared");
    }
    const auto found = std::find_if(manifest.resources.begin(),
      manifest.resources.end(),
      [&](const auto& resource) { return resource.id == instruction; });
    if (found->kind != skill_resource_kind::instructions &&
        found->kind != skill_resource_kind::prompt) {
      throw std::invalid_argument("instruction resource must have instructions or prompt kind");
    }
  }

  std::set<std::string> dependency_ids;
  for (const auto& dependency : manifest.dependencies) {
    if (!valid_id(dependency.id) || dependency.id == manifest.descriptor.id) {
      throw std::invalid_argument("skill dependency has an invalid or self-referential id");
    }
    if (!dependency_ids.insert(dependency.id).second) {
      throw std::invalid_argument("skill dependencies must have unique ids");
    }
  }
  std::set<std::string> tool_names;
  for (const auto& tool : manifest.tools) {
    if (tool.name.empty() || !tool_names.insert(tool.name).second) {
      throw std::invalid_argument("skill tool requirements must have unique non-empty names");
    }
    if (tool.exact_version) {
      if (tool.exact_version->empty() || tool.exact_version->size() > 255) {
        throw std::invalid_argument("skill tool exact_version must be a non-empty opaque string");
      }
    }
  }
  std::set<std::string> capability_names;
  for (const auto& capability : manifest.capabilities) {
    if (capability.name.empty() || !capability_names.insert(capability.name).second) {
      throw std::invalid_argument("skill capability requirements must have unique non-empty names");
    }
  }
  std::set<std::string> knowledge_sources;
  for (const auto& knowledge : manifest.knowledge) {
    if (knowledge.source.empty() || !knowledge_sources.insert(knowledge.source).second) {
      throw std::invalid_argument(
        "skill knowledge requirements must have unique non-empty sources");
    }
  }
}

} // namespace wuwe::agent::skills
