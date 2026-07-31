#ifndef WUWE_AGENT_CORE_METADATA_HPP
#define WUWE_AGENT_CORE_METADATA_HPP

#include <cctype>
#include <string>
#include <string_view>

namespace wuwe::agent::core {

[[nodiscard]] inline bool sensitive_metadata_key(std::string_view key) noexcept {
  std::string normalized;
  normalized.reserve(key.size() + 4);
  bool previous_was_lower_or_digit = false;
  for (const auto raw_ch : key) {
    const auto ch = static_cast<unsigned char>(raw_ch);
    if (std::isalnum(ch)) {
      if (std::isupper(ch) && previous_was_lower_or_digit && !normalized.empty() &&
          normalized.back() != '_') {
        normalized.push_back('_');
      }
      normalized.push_back(static_cast<char>(std::tolower(ch)));
      previous_was_lower_or_digit = std::islower(ch) || std::isdigit(ch);
      continue;
    }
    if (!normalized.empty() && normalized.back() != '_') {
      normalized.push_back('_');
    }
    previous_was_lower_or_digit = false;
  }
  while (!normalized.empty() && normalized.back() == '_') {
    normalized.pop_back();
  }
  constexpr std::string_view sensitive_names[] {
    "token",
    "secret",
    "password",
    "credential",
    "credentials",
    "authorization",
    "proxy_authorization",
    "api_key",
    "apikey",
    "access_key",
    "private_key",
    "secret_key",
    "cookie",
    "set_cookie",
  };
  for (const auto name : sensitive_names) {
    if (normalized == name) {
      return true;
    }
  }
  constexpr std::string_view sensitive_suffixes[] {
    "_token",
    ".token",
    "_secret",
    ".secret",
    "_password",
    ".password",
    "_credential",
    ".credential",
    "_credentials",
    ".credentials",
    "_authorization",
    ".authorization",
    "_api_key",
    ".api_key",
    "_access_key",
    ".access_key",
    "_private_key",
    ".private_key",
    "_secret_key",
    ".secret_key",
    "_cookie",
    ".cookie",
  };
  for (const auto suffix : sensitive_suffixes) {
    if (normalized.size() >= suffix.size() &&
        normalized.compare(normalized.size() - suffix.size(), suffix.size(), suffix) == 0) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] inline bool sensitive_execution_context_metadata_key(std::string_view key) noexcept {
  return sensitive_metadata_key(key);
}

} // namespace wuwe::agent::core

#endif // WUWE_AGENT_CORE_METADATA_HPP
