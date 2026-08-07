#ifndef WUWE_AGENT_LLM_TOOL_OUTPUT_PROJECTION_TYPES_HPP
#define WUWE_AGENT_LLM_TOOL_OUTPUT_PROJECTION_TYPES_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace wuwe::agent::llm {

inline constexpr std::size_t default_tool_output_projection_max_bytes = 64 * 1024;
inline constexpr std::size_t default_tool_output_projection_max_tokens = 10'000;
inline constexpr std::size_t minimum_tool_output_projection_max_bytes = 256;
inline constexpr std::size_t minimum_tool_output_projection_max_tokens = 64;

struct tool_output_projection_policy {
  std::size_t max_bytes { default_tool_output_projection_max_bytes };
  std::size_t max_tokens { default_tool_output_projection_max_tokens };
};

struct tool_output_projection_constraints {
  std::optional<std::size_t> max_bytes;
  std::optional<std::size_t> max_tokens;
};

enum class tool_output_projection_limit {
  none,
  bytes,
  tokens,
  bytes_and_tokens,
};

[[nodiscard]] std::string_view to_string(tool_output_projection_limit limit) noexcept;

enum class tool_output_projection_error {
  none,
  invalid_policy,
  envelope_exceeds_limits,
  estimator_failure,
  serialization_failure,
};

[[nodiscard]] std::string_view to_string(tool_output_projection_error error) noexcept;

struct tool_output_projection_report {
  bool truncated { false };
  std::size_t original_bytes { 0 };
  std::size_t projected_bytes { 0 };
  std::size_t original_estimated_tokens { 0 };
  std::size_t projected_estimated_tokens { 0 };
  std::size_t max_bytes { 0 };
  std::size_t max_tokens { 0 };
  tool_output_projection_limit limiting_factor { tool_output_projection_limit::none };
};

struct tool_output_projection {
  std::string content;
  tool_output_projection_report report;
};

struct tool_output_projection_policy_validation {
  tool_output_projection_error error { tool_output_projection_error::none };
  std::string message;

  explicit operator bool() const noexcept {
    return error == tool_output_projection_error::none;
  }
};

struct tool_output_projection_result {
  tool_output_projection projection;
  tool_output_projection_error error { tool_output_projection_error::none };
  std::string message;

  explicit operator bool() const noexcept {
    return error == tool_output_projection_error::none;
  }
};

} // namespace wuwe::agent::llm

#endif // WUWE_AGENT_LLM_TOOL_OUTPUT_PROJECTION_TYPES_HPP
