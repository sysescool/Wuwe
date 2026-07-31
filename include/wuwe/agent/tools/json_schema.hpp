#ifndef WUWE_AGENT_TOOLS_JSON_SCHEMA_HPP
#define WUWE_AGENT_TOOLS_JSON_SCHEMA_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace wuwe::agent::tools {

struct json_schema_issue {
  std::string instance_path;
  std::string schema_path;
  std::string message;
};

struct json_schema_validation_result {
  std::vector<json_schema_issue> issues;
  [[nodiscard]] bool valid() const noexcept {
    return issues.empty();
  }
  explicit operator bool() const noexcept {
    return valid();
  }
};

class json_schema_validator {
public:
  [[nodiscard]] json_schema_validation_result validate(
    const nlohmann::json& instance, const nlohmann::json& schema) const {
    json_schema_validation_result result;
    validate_node(instance, schema, schema, "", "", 0, result);
    return result;
  }

private:
  static constexpr std::size_t maximum_depth = 128;

  static std::string escape_pointer(std::string_view value) {
    std::string output;
    for (const char ch : value) {
      if (ch == '~')
        output += "~0";
      else if (ch == '/')
        output += "~1";
      else
        output.push_back(ch);
    }
    return output;
  }

  static void add_issue(json_schema_validation_result& result, std::string instance_path,
    std::string schema_path, std::string message) {
    result.issues.push_back({
      .instance_path = instance_path.empty() ? "/" : std::move(instance_path),
      .schema_path = schema_path.empty() ? "/" : std::move(schema_path),
      .message = std::move(message),
    });
  }

  static bool matches_type(const nlohmann::json& value, std::string_view type) {
    if (type == "null")
      return value.is_null();
    if (type == "boolean")
      return value.is_boolean();
    if (type == "object")
      return value.is_object();
    if (type == "array")
      return value.is_array();
    if (type == "number")
      return value.is_number();
    if (type == "integer") {
      if (value.is_number_integer() || value.is_number_unsigned())
        return true;
      return value.is_number_float() && std::floor(value.get<double>()) == value.get<double>();
    }
    if (type == "string")
      return value.is_string();
    return false;
  }

  static std::optional<std::size_t> nonnegative_size(const nlohmann::json& value) {
    if (value.is_number_unsigned())
      return value.get<std::size_t>();
    if (value.is_number_integer()) {
      const auto number = value.get<std::int64_t>();
      if (number >= 0)
        return static_cast<std::size_t>(number);
    }
    return std::nullopt;
  }

  static bool validate_type_keyword(const nlohmann::json& instance, const nlohmann::json& keyword) {
    if (keyword.is_string()) {
      return matches_type(instance, keyword.get_ref<const std::string&>());
    }
    if (keyword.is_array()) {
      for (const auto& item : keyword) {
        if (item.is_string() && matches_type(instance, item.get_ref<const std::string&>())) {
          return true;
        }
      }
    }
    return false;
  }

  static const nlohmann::json* resolve_local_ref(
    const nlohmann::json& root, const std::string& reference) {
    if (reference == "#")
      return &root;
    if (!reference.starts_with("#/"))
      return nullptr;
    try {
      return &root.at(nlohmann::json::json_pointer(reference.substr(1)));
    }
    catch (...) {
      return nullptr;
    }
  }

  void validate_node(const nlohmann::json& instance, const nlohmann::json& schema,
    const nlohmann::json& root, const std::string& instance_path, const std::string& schema_path,
    std::size_t depth, json_schema_validation_result& result) const {
    if (depth > maximum_depth) {
      add_issue(result, instance_path, schema_path, "schema recursion depth exceeded");
      return;
    }
    if (schema.is_boolean()) {
      if (!schema.get<bool>()) {
        add_issue(result, instance_path, schema_path, "value is rejected by a false schema");
      }
      return;
    }
    if (!schema.is_object()) {
      add_issue(result, instance_path, schema_path, "schema must be an object or boolean");
      return;
    }
    static constexpr const char* unsupported_assertions[] {
      "patternProperties",
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
      "else",
    };
    for (const auto* keyword : unsupported_assertions) {
      if (schema.contains(keyword)) {
        add_issue(result,
          instance_path,
          schema_path + "/" + keyword,
          std::string("unsupported JSON Schema assertion: ") + keyword);
      }
    }

    if (const auto ref = schema.find("$ref"); ref != schema.end()) {
      if (!ref->is_string()) {
        add_issue(result, instance_path, schema_path + "/$ref", "$ref must be a string");
        return;
      }
      const auto* target = resolve_local_ref(root, ref->get_ref<const std::string&>());
      if (!target) {
        add_issue(result,
          instance_path,
          schema_path + "/$ref",
          "only valid local JSON Pointer references are supported");
        return;
      }
      validate_node(instance,
        *target,
        root,
        instance_path,
        ref->get_ref<const std::string&>(),
        depth + 1,
        result);
    }

    if (const auto type = schema.find("type");
        type != schema.end() && !validate_type_keyword(instance, *type)) {
      add_issue(
        result, instance_path, schema_path + "/type", "value does not match the declared type");
      return;
    }
    if (instance.is_number_float() && !std::isfinite(instance.get<double>())) {
      add_issue(result, instance_path, schema_path, "JSON numbers must be finite");
      return;
    }
    if (const auto constant = schema.find("const");
        constant != schema.end() && instance != *constant) {
      add_issue(result, instance_path, schema_path + "/const", "value does not match const");
    }
    if (const auto values = schema.find("enum"); values != schema.end()) {
      if (!values->is_array()) {
        add_issue(result, instance_path, schema_path + "/enum", "enum must be an array");
      }
      else {
        bool found = false;
        for (const auto& value : *values)
          found = found || value == instance;
        if (!found)
          add_issue(result, instance_path, schema_path + "/enum", "value is not present in enum");
      }
    }

    validate_combinators(instance, schema, root, instance_path, schema_path, depth, result);
    if (instance.is_object()) {
      validate_object(instance, schema, root, instance_path, schema_path, depth, result);
    }
    if (instance.is_array()) {
      validate_array(instance, schema, root, instance_path, schema_path, depth, result);
    }
    if (instance.is_string()) {
      validate_string(
        instance.get_ref<const std::string&>(), schema, instance_path, schema_path, result);
    }
    if (instance.is_number()) {
      validate_number(instance.get<double>(), schema, instance_path, schema_path, result);
    }
  }

  void validate_combinators(const nlohmann::json& instance, const nlohmann::json& schema,
    const nlohmann::json& root, const std::string& instance_path, const std::string& schema_path,
    std::size_t depth, json_schema_validation_result& result) const {
    for (const auto* keyword : { "allOf", "anyOf", "oneOf" }) {
      const auto alternatives = schema.find(keyword);
      if (alternatives == schema.end())
        continue;
      if (!alternatives->is_array()) {
        add_issue(result,
          instance_path,
          schema_path + "/" + keyword,
          std::string(keyword) + " must be an array");
        continue;
      }
      std::size_t matches = 0;
      for (std::size_t index = 0; index < alternatives->size(); ++index) {
        json_schema_validation_result nested;
        validate_node(instance,
          alternatives->at(index),
          root,
          instance_path,
          schema_path + "/" + keyword + "/" + std::to_string(index),
          depth + 1,
          nested);
        if (nested.valid())
          ++matches;
      }
      const bool valid = std::string_view(keyword) == "allOf"   ? matches == alternatives->size()
                         : std::string_view(keyword) == "anyOf" ? matches > 0
                                                                : matches == 1;
      if (!valid)
        add_issue(result,
          instance_path,
          schema_path + "/" + keyword,
          std::string("value does not satisfy ") + keyword);
    }
    if (const auto negated = schema.find("not"); negated != schema.end()) {
      json_schema_validation_result nested;
      validate_node(
        instance, *negated, root, instance_path, schema_path + "/not", depth + 1, nested);
      if (nested.valid())
        add_issue(result, instance_path, schema_path + "/not", "value satisfies a negated schema");
    }
  }

  void validate_object(const nlohmann::json& instance, const nlohmann::json& schema,
    const nlohmann::json& root, const std::string& instance_path, const std::string& schema_path,
    std::size_t depth, json_schema_validation_result& result) const {
    validate_size_keyword(instance.size(),
      schema,
      "minProperties",
      true,
      instance_path,
      schema_path,
      "object has too few properties",
      result);
    validate_size_keyword(instance.size(),
      schema,
      "maxProperties",
      false,
      instance_path,
      schema_path,
      "object has too many properties",
      result);
    if (const auto required = schema.find("required"); required != schema.end()) {
      if (!required->is_array()) {
        add_issue(result, instance_path, schema_path + "/required", "required must be an array");
      }
      else
        for (const auto& property : *required) {
          if (!property.is_string()) {
            add_issue(
              result, instance_path, schema_path + "/required", "required entries must be strings");
          }
          else if (!instance.contains(property.get_ref<const std::string&>())) {
            add_issue(result,
              instance_path,
              schema_path + "/required",
              "required property is missing: " + property.get<std::string>());
          }
        }
    }

    const auto properties = schema.find("properties");
    std::set<std::string> evaluated;
    if (properties != schema.end()) {
      if (!properties->is_object()) {
        add_issue(
          result, instance_path, schema_path + "/properties", "properties must be an object");
      }
      else
        for (const auto& [name, child_schema] : properties->items()) {
          if (!instance.contains(name))
            continue;
          evaluated.insert(name);
          validate_node(instance.at(name),
            child_schema,
            root,
            instance_path + "/" + escape_pointer(name),
            schema_path + "/properties/" + escape_pointer(name),
            depth + 1,
            result);
        }
    }
    const auto additional = schema.find("additionalProperties");
    if (additional != schema.end()) {
      if (!additional->is_object() && !additional->is_boolean()) {
        add_issue(result,
          instance_path,
          schema_path + "/additionalProperties",
          "additionalProperties must be an object or boolean");
        return;
      }
      for (const auto& [name, value] : instance.items()) {
        if (evaluated.contains(name))
          continue;
        if (additional->is_boolean() && !additional->get<bool>()) {
          add_issue(result,
            instance_path + "/" + escape_pointer(name),
            schema_path + "/additionalProperties",
            "additional property is not allowed");
        }
        else if (additional->is_object() || additional->is_boolean()) {
          validate_node(value,
            *additional,
            root,
            instance_path + "/" + escape_pointer(name),
            schema_path + "/additionalProperties",
            depth + 1,
            result);
        }
      }
    }
  }

  void validate_array(const nlohmann::json& instance, const nlohmann::json& schema,
    const nlohmann::json& root, const std::string& instance_path, const std::string& schema_path,
    std::size_t depth, json_schema_validation_result& result) const {
    validate_size_keyword(instance.size(),
      schema,
      "minItems",
      true,
      instance_path,
      schema_path,
      "array has too few items",
      result);
    validate_size_keyword(instance.size(),
      schema,
      "maxItems",
      false,
      instance_path,
      schema_path,
      "array has too many items",
      result);
    const auto unique = schema.find("uniqueItems");
    if (unique != schema.end() && !unique->is_boolean()) {
      add_issue(
        result, instance_path, schema_path + "/uniqueItems", "uniqueItems must be a boolean");
    }
    else if (unique != schema.end() && unique->get<bool>()) {
      for (std::size_t left = 0; left < instance.size(); ++left) {
        for (std::size_t right = left + 1; right < instance.size(); ++right) {
          if (instance[left] == instance[right]) {
            add_issue(result,
              instance_path,
              schema_path + "/uniqueItems",
              "array contains duplicate items");
            left = instance.size();
            break;
          }
        }
      }
    }
    if (const auto items = schema.find("items"); items != schema.end()) {
      for (std::size_t index = 0; index < instance.size(); ++index) {
        validate_node(instance[index],
          *items,
          root,
          instance_path + "/" + std::to_string(index),
          schema_path + "/items",
          depth + 1,
          result);
      }
    }
  }

  static void validate_string(const std::string& instance, const nlohmann::json& schema,
    const std::string& instance_path, const std::string& schema_path,
    json_schema_validation_result& result) {
    const auto length = utf8_length(instance);
    validate_size_keyword(
      length, schema, "minLength", true, instance_path, schema_path, "string is too short", result);
    validate_size_keyword(
      length, schema, "maxLength", false, instance_path, schema_path, "string is too long", result);
    if (const auto pattern = schema.find("pattern"); pattern != schema.end()) {
      if (!pattern->is_string()) {
        add_issue(result, instance_path, schema_path + "/pattern", "pattern must be a string");
      }
      else
        try {
          if (!std::regex_search(instance, std::regex(pattern->get<std::string>()))) {
            add_issue(
              result, instance_path, schema_path + "/pattern", "string does not match pattern");
          }
        }
        catch (const std::regex_error&) {
          add_issue(result, instance_path, schema_path + "/pattern", "pattern is invalid");
        }
    }
  }

  static std::size_t utf8_length(std::string_view value) {
    std::size_t length = 0;
    for (const auto byte : value) {
      if ((static_cast<unsigned char>(byte) & 0xc0) != 0x80)
        ++length;
    }
    return length;
  }

  static void validate_size_keyword(std::size_t actual, const nlohmann::json& schema,
    const char* keyword, bool minimum, const std::string& instance_path,
    const std::string& schema_path, const char* violation, json_schema_validation_result& result) {
    const auto value = schema.find(keyword);
    if (value == schema.end())
      return;
    const auto limit = nonnegative_size(*value);
    if (!limit) {
      add_issue(result,
        instance_path,
        schema_path + "/" + keyword,
        std::string(keyword) + " must be a non-negative integer");
      return;
    }
    if ((minimum && actual < *limit) || (!minimum && actual > *limit)) {
      add_issue(result, instance_path, schema_path + "/" + keyword, violation);
    }
  }

  static void validate_number(double instance, const nlohmann::json& schema,
    const std::string& instance_path, const std::string& schema_path,
    json_schema_validation_result& result) {
    const auto compare = [&](const char* keyword, auto predicate, const char* message) {
      const auto value = schema.find(keyword);
      if (value == schema.end())
        return;
      if (!value->is_number()) {
        add_issue(result,
          instance_path,
          schema_path + "/" + keyword,
          std::string(keyword) + " must be a number");
      }
      else if (!std::isfinite(value->get<double>())) {
        add_issue(result,
          instance_path,
          schema_path + "/" + keyword,
          std::string(keyword) + " must be finite");
      }
      else if (!predicate(instance, value->get<double>())) {
        add_issue(result, instance_path, schema_path + "/" + keyword, message);
      }
    };
    compare("minimum", std::greater_equal<double> {}, "number is below minimum");
    compare("maximum", std::less_equal<double> {}, "number is above maximum");
    compare("exclusiveMinimum", std::greater<double> {}, "number is not above exclusiveMinimum");
    compare("exclusiveMaximum", std::less<double> {}, "number is not below exclusiveMaximum");
    if (const auto multiple = schema.find("multipleOf"); multiple != schema.end()) {
      if (!multiple->is_number() || !std::isfinite(multiple->get<double>()) ||
          multiple->get<double>() <= 0.0) {
        add_issue(result,
          instance_path,
          schema_path + "/multipleOf",
          "multipleOf must be a positive number");
      }
      else {
        const auto divisor = multiple->get<double>();
        const auto quotient = instance / divisor;
        if (std::abs(quotient - std::round(quotient)) > 1e-9) {
          add_issue(result,
            instance_path,
            schema_path + "/multipleOf",
            "number is not a multiple of the declared value");
        }
      }
    }
  }
};

} // namespace wuwe::agent::tools

#endif // WUWE_AGENT_TOOLS_JSON_SCHEMA_HPP
