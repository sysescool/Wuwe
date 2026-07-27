#ifndef WUWE_AGENT_CORE_SQLITE_SCHEMA_HPP
#define WUWE_AGENT_CORE_SQLITE_SCHEMA_HPP

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <unordered_map>

#if WUWE_HAS_SQLITE
#include <sqlite3.h>
#endif

namespace wuwe::agent::core {

#if WUWE_HAS_SQLITE

struct sqlite_column_contract {
  const char* name {};
  const char* declared_type {};
  bool not_null { false };
  int primary_key_position { 0 };
};

namespace detail {

struct sqlite_column_info {
  std::string declared_type;
  bool not_null { false };
  int primary_key_position { 0 };
};

[[nodiscard]] inline std::string normalized_sqlite_type(std::string value) {
  value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char character) {
    return std::isspace(character) != 0;
  }), value.end());
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  return value;
}

[[nodiscard]] inline std::string sqlite_type_affinity(const std::string& declared_type) {
  const auto type = normalized_sqlite_type(declared_type);
  if (type.find("INT") != std::string::npos) return "INTEGER";
  if (type.find("CHAR") != std::string::npos ||
      type.find("CLOB") != std::string::npos ||
      type.find("TEXT") != std::string::npos) {
    return "TEXT";
  }
  if (type.empty() || type.find("BLOB") != std::string::npos) return "BLOB";
  if (type.find("REAL") != std::string::npos ||
      type.find("FLOA") != std::string::npos ||
      type.find("DOUB") != std::string::npos) {
    return "REAL";
  }
  return "NUMERIC";
}

[[nodiscard]] inline std::string sqlite_column_text(sqlite3_stmt* statement, int column) {
  const auto* value = sqlite3_column_text(statement, column);
  return value ? reinterpret_cast<const char*>(value) : std::string {};
}

} // namespace detail

inline void validate_sqlite_table(
  sqlite3* db,
  const std::string& table,
  std::initializer_list<sqlite_column_contract> required_columns,
  const std::string& component) {
  if (!db) {
    throw std::invalid_argument("validate_sqlite_table requires an open database");
  }
  if (table.empty()) {
    throw std::invalid_argument("validate_sqlite_table requires a table name");
  }
  if (table.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") !=
      std::string::npos) {
    throw std::invalid_argument("invalid SQLite table name: " + table);
  }

  sqlite3_stmt* raw_statement {};
  const auto sql = "PRAGMA table_info(" + table + ")";
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw_statement, nullptr) != SQLITE_OK) {
    throw std::runtime_error(
      component + " failed to inspect table " + table + ": " + sqlite3_errmsg(db));
  }
  struct statement_guard {
    sqlite3_stmt* value {};
    ~statement_guard() { if (value) sqlite3_finalize(value); }
  } guard { raw_statement };

  std::unordered_map<std::string, detail::sqlite_column_info> columns;
  while (true) {
    const auto rc = sqlite3_step(raw_statement);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) {
      throw std::runtime_error(
        component + " failed to inspect table " + table + ": " + sqlite3_errmsg(db));
    }
    columns.emplace(detail::sqlite_column_text(raw_statement, 1),
      detail::sqlite_column_info {
        .declared_type = detail::normalized_sqlite_type(
          detail::sqlite_column_text(raw_statement, 2)),
        .not_null = sqlite3_column_int(raw_statement, 3) != 0,
        .primary_key_position = sqlite3_column_int(raw_statement, 5),
      });
  }

  if (columns.empty()) {
    throw std::runtime_error(component + " is missing required table " + table);
  }
  for (const auto& required : required_columns) {
    const auto found = columns.find(required.name ? required.name : "");
    if (found == columns.end()) {
      throw std::runtime_error(
        component + " table " + table + " is missing required column " +
        (required.name ? required.name : "<unnamed>"));
    }
    const auto expected_type = detail::sqlite_type_affinity(
      required.declared_type ? required.declared_type : "");
    const auto actual_type = detail::sqlite_type_affinity(found->second.declared_type);
    if (required.declared_type && *required.declared_type != '\0' &&
        actual_type != expected_type) {
      throw std::runtime_error(
        component + " table " + table + " column " + required.name +
        " has " + actual_type + " affinity, expected " + expected_type);
    }
    if (required.not_null && !found->second.not_null &&
        found->second.primary_key_position == 0) {
      throw std::runtime_error(
        component + " table " + table + " column " + required.name +
        " must be NOT NULL");
    }
    if (found->second.primary_key_position != required.primary_key_position) {
      throw std::runtime_error(
        component + " table " + table + " column " + required.name +
        " has an incompatible primary-key position");
    }
  }
  for (const auto& [name, column] : columns) {
    if (column.primary_key_position == 0) continue;
    const auto expected = std::find_if(required_columns.begin(), required_columns.end(),
      [&](const sqlite_column_contract& contract) {
        return contract.name && name == contract.name &&
          contract.primary_key_position == column.primary_key_position;
      });
    if (expected == required_columns.end()) {
      throw std::runtime_error(
        component + " table " + table + " has unexpected primary-key column " + name);
    }
  }
}

#endif // WUWE_HAS_SQLITE

} // namespace wuwe::agent::core

#endif // WUWE_AGENT_CORE_SQLITE_SCHEMA_HPP
