#ifndef WUWE_AGENT_GUARDRAILS_TEXT_GUARDRAIL_HPP
#define WUWE_AGENT_GUARDRAILS_TEXT_GUARDRAIL_HPP

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wuwe/agent/guardrails/guardrail_core.hpp>

namespace wuwe::agent::guardrails {

struct text_guardrail_options {
  std::string name { "text_policy" };
  std::set<guardrail_stage> stages { guardrail_stage::input, guardrail_stage::output };
  std::vector<std::string> denied_terms;
  std::vector<std::string> redacted_terms;
  std::size_t max_characters { 0 };
  std::size_t max_bytes { 0 };
  bool case_sensitive { false };
  std::string redaction { "[REDACTED]" };
};

class text_guardrail final : public guardrail {
public:
  explicit text_guardrail(text_guardrail_options options)
      : options_(std::move(options)) {
    if (options_.name.empty()) {
      throw std::invalid_argument("text_guardrail requires a name");
    }
  }

  [[nodiscard]] std::string name() const override {
    return options_.name;
  }

  guardrail_result evaluate(const guardrail_request& request) const override {
    if (!options_.stages.contains(request.stage)) {
      return guardrail_result::allow();
    }
    if (options_.max_bytes != 0 && request.content.size() > options_.max_bytes) {
      return guardrail_result::deny({
        .severity = guardrail_severity::error,
        .code = "content_too_large",
        .message = "content exceeds the configured byte limit",
        .remediation = "shorten the content before retrying",
        .metadata = { { "max_bytes", std::to_string(options_.max_bytes) } },
      });
    }
    if (options_.max_characters != 0) {
      const auto characters = utf8_code_point_count(request.content);
      if (!characters) {
        return guardrail_result::deny({
          .severity = guardrail_severity::error,
          .code = "invalid_utf8",
          .message = "content is not valid UTF-8",
          .remediation = "provide valid UTF-8 content",
        });
      }
      if (*characters <= options_.max_characters) {
        return evaluate_terms(request);
      }
      return guardrail_result::deny({
        .severity = guardrail_severity::error,
        .code = "content_too_long",
        .message = "content exceeds the configured character limit",
        .remediation = "shorten the content before retrying",
        .metadata = { { "max_characters", std::to_string(options_.max_characters) } },
      });
    }
    return evaluate_terms(request);
  }

private:
  guardrail_result evaluate_terms(const guardrail_request& request) const {
    for (const auto& term : options_.denied_terms) {
      if (!term.empty() && find(request.content, term) != std::string::npos) {
        return guardrail_result::deny({
          .severity = guardrail_severity::error,
          .code = "denied_term",
          .message = "content matched a denied term",
          .remediation = "remove the prohibited content",
          .metadata = { { "term", term } },
        });
      }
    }

    auto output = request.content;
    bool modified = false;
    for (const auto& term : options_.redacted_terms) {
      if (term.empty()) {
        continue;
      }
      std::size_t position = 0;
      while ((position = find(output, term, position)) != std::string::npos) {
        output.replace(position, term.size(), options_.redaction);
        position += options_.redaction.size();
        modified = true;
      }
    }
    if (!modified) {
      return guardrail_result::allow();
    }
    return guardrail_result::modify(std::move(output), {
      .severity = guardrail_severity::warning,
      .code = "content_redacted",
      .message = "sensitive content was redacted",
    });
  }

  static std::optional<std::size_t> utf8_code_point_count(std::string_view value) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < value.size();) {
      const auto lead = static_cast<unsigned char>(value[index]);
      std::size_t length = 0;
      if (lead <= 0x7f) {
        length = 1;
      }
      else if (lead >= 0xc2 && lead <= 0xdf) {
        length = 2;
      }
      else if (lead >= 0xe0 && lead <= 0xef) {
        length = 3;
      }
      else if (lead >= 0xf0 && lead <= 0xf4) {
        length = 4;
      }
      else {
        return std::nullopt;
      }
      if (index + length > value.size()) {
        return std::nullopt;
      }
      for (std::size_t offset = 1; offset < length; ++offset) {
        const auto continuation = static_cast<unsigned char>(value[index + offset]);
        if ((continuation & 0xc0) != 0x80) {
          return std::nullopt;
        }
      }
      if (length == 3) {
        const auto second = static_cast<unsigned char>(value[index + 1]);
        if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0)) {
          return std::nullopt;
        }
      }
      if (length == 4) {
        const auto second = static_cast<unsigned char>(value[index + 1]);
        if ((lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90)) {
          return std::nullopt;
        }
      }
      index += length;
      ++count;
    }
    return count;
  }

  [[nodiscard]] std::size_t find(
    std::string_view value,
    std::string_view needle,
    std::size_t offset = 0) const {
    if (options_.case_sensitive) {
      return value.find(needle, offset);
    }
    if (needle.empty() || value.size() < needle.size() || offset >= value.size()) {
      return std::string::npos;
    }
    for (std::size_t index = offset; index + needle.size() <= value.size(); ++index) {
      bool matches = true;
      for (std::size_t character = 0; character < needle.size(); ++character) {
        const auto lhs = static_cast<unsigned char>(value[index + character]);
        const auto rhs = static_cast<unsigned char>(needle[character]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
          matches = false;
          break;
        }
      }
      if (matches) {
        return index;
      }
    }
    return std::string::npos;
  }

  text_guardrail_options options_;
};

} // namespace wuwe::agent::guardrails

#endif // WUWE_AGENT_GUARDRAILS_TEXT_GUARDRAIL_HPP
