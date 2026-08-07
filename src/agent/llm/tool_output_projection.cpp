#include <wuwe/agent/llm/tool_output_projection.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include <wuwe/agent/tools/tool_contract.hpp>

namespace wuwe::agent::llm {

namespace {

inline constexpr std::string_view output_omission_marker = "\n... tool output omitted ...\n";
inline constexpr std::size_t preview_search_attempts = 32;
inline constexpr std::size_t preview_decay_numerator = 3;
inline constexpr std::size_t preview_decay_denominator = 4;

[[nodiscard]] std::string canonical_tool_output_for_model(const tools::tool_outcome& result) {
  if (result.error_code || result.error_category != tools::tool_error_category::none) {
    return nlohmann::json(
      {
        { "ok", false },
        { "error",
          {
            { "category", tools::to_string(result.error_category) },
            { "code", result.error_code.value() },
            { "message", result.content.empty() ? result.error_code.message() : result.content },
            { "retryable", result.retryable },
          } },
      })
      .dump();
  }
  if (!result.data.is_null() && !result.data.empty()) {
    return nlohmann::json({
                            { "ok", true },
                            { "message", result.content },
                            { "data", result.data },
                            { "artifacts", result.artifacts },
                          })
      .dump();
  }
  return result.content;
}

[[nodiscard]] bool structured_tool_output(const tools::tool_outcome& result) noexcept {
  return result.error_code || result.error_category != tools::tool_error_category::none ||
         (!result.data.is_null() && !result.data.empty());
}

[[nodiscard]] std::string_view utf8_prefix(std::string_view text, std::size_t bytes) {
  bytes = (std::min)(bytes, text.size());
  while (bytes > 0 && bytes < text.size() &&
         (static_cast<unsigned char>(text[bytes]) & 0xc0U) == 0x80U) {
    --bytes;
  }
  return text.substr(0, bytes);
}

[[nodiscard]] std::string_view utf8_suffix(std::string_view text, std::size_t bytes) {
  bytes = (std::min)(bytes, text.size());
  auto start = text.size() - bytes;
  while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xc0U) == 0x80U) {
    ++start;
  }
  return text.substr(start);
}

[[nodiscard]] std::string truncate_middle_bytes(std::string_view text, std::size_t byte_limit) {
  if (text.size() <= byte_limit) {
    return std::string(text);
  }
  if (byte_limit <= output_omission_marker.size()) {
    return {};
  }
  const auto content_bytes = byte_limit - output_omission_marker.size();
  const auto head_bytes = content_bytes / 2;
  const auto tail_bytes = content_bytes - head_bytes;
  auto output = std::string(utf8_prefix(text, head_bytes));
  output.append(output_omission_marker);
  output.append(utf8_suffix(text, tail_bytes));
  return output;
}

[[nodiscard]] std::string bounded_middle_preview(std::string_view text, std::size_t byte_limit,
  std::size_t token_limit, const text_token_estimator& estimator) {
  if (byte_limit == 0 || token_limit == 0 || text.empty()) {
    return {};
  }

  const auto prelimit =
    byte_limit > (std::numeric_limits<std::size_t>::max)() / 2 ? byte_limit : byte_limit * 2;
  auto bounded_source = truncate_middle_bytes(text, prelimit);
  if (bounded_source.empty()) {
    return {};
  }
  if (bounded_source.size() <= byte_limit &&
      estimator.estimate_text(bounded_source) <= token_limit) {
    return bounded_source;
  }

  const auto marker_tokens = estimator.estimate_text(output_omission_marker);
  auto candidate_tokens = token_limit;
  for (std::size_t attempt = 0;
       attempt < preview_search_attempts && candidate_tokens > marker_tokens;
       ++attempt) {
    const auto content_tokens = candidate_tokens - marker_tokens;
    const auto head_tokens = content_tokens / 2;
    const auto tail_tokens = content_tokens - head_tokens;
    auto candidate = estimator.truncate_text(bounded_source, head_tokens, false);
    candidate.append(output_omission_marker);
    candidate.append(estimator.truncate_text(bounded_source, tail_tokens, true));
    if (candidate.size() <= byte_limit && estimator.estimate_text(candidate) <= token_limit) {
      return candidate;
    }
    const auto reduced = candidate_tokens * preview_decay_numerator / preview_decay_denominator;
    candidate_tokens = reduced < candidate_tokens ? reduced : candidate_tokens - 1;
  }
  return {};
}

[[nodiscard]] tool_output_projection_limit limiting_factor(
  bool byte_limited, bool token_limited) noexcept {
  if (byte_limited && token_limited) {
    return tool_output_projection_limit::bytes_and_tokens;
  }
  if (byte_limited) {
    return tool_output_projection_limit::bytes;
  }
  if (token_limited) {
    return tool_output_projection_limit::tokens;
  }
  return tool_output_projection_limit::none;
}

[[nodiscard]] std::string plain_projection_envelope(
  std::string_view preview, std::size_t original_bytes, std::size_t original_tokens) {
  std::string output = "Warning: tool output truncated (original bytes: ";
  output += std::to_string(original_bytes);
  output += ", estimated tokens: ";
  output += std::to_string(original_tokens);
  output += ").\n";
  output.append(preview);
  return output;
}

[[nodiscard]] std::string structured_projection_envelope(const tools::tool_outcome& result,
  std::string_view preview, std::size_t original_bytes, std::size_t original_tokens) {
  nlohmann::json output {
    { "ok", result.succeeded() },
    { "preview", preview },
    { "truncation",
      {
        { "truncated", true },
        { "original_bytes", original_bytes },
        { "original_estimated_tokens", original_tokens },
      } },
  };
  if (!result.succeeded()) {
    output["error"] = {
      { "category", tools::to_string(result.error_category) },
      { "code", result.error_code.value() },
      { "retryable", result.retryable },
    };
  }
  return output.dump();
}

[[nodiscard]] tool_output_projection_result projection_failure(
  tool_output_projection_error error, std::string message) {
  return {
    .error = error,
    .message = std::move(message),
  };
}

[[nodiscard]] tool_output_projection_policy_validation policy_failure(
  tool_output_projection_error error, std::string message) {
  return {
    .error = error,
    .message = std::move(message),
  };
}

[[nodiscard]] bool envelope_fits(std::string_view envelope,
  const tool_output_projection_policy& policy, const text_token_estimator& estimator) {
  return envelope.size() <= policy.max_bytes &&
         estimator.estimate_text(envelope) <= policy.max_tokens;
}

} // namespace

std::string_view to_string(tool_output_projection_limit limit) noexcept {
  switch (limit) {
    case tool_output_projection_limit::none:
      return "none";
    case tool_output_projection_limit::bytes:
      return "bytes";
    case tool_output_projection_limit::tokens:
      return "tokens";
    case tool_output_projection_limit::bytes_and_tokens:
      return "bytes_and_tokens";
  }
  return "none";
}

std::string_view to_string(tool_output_projection_error error) noexcept {
  switch (error) {
    case tool_output_projection_error::none:
      return "none";
    case tool_output_projection_error::invalid_policy:
      return "invalid_policy";
    case tool_output_projection_error::envelope_exceeds_limits:
      return "envelope_exceeds_limits";
    case tool_output_projection_error::estimator_failure:
      return "estimator_failure";
    case tool_output_projection_error::serialization_failure:
      return "serialization_failure";
  }
  return "none";
}

void validate_tool_output_projection_policy(const tool_output_projection_policy& policy) {
  if (policy.max_bytes < minimum_tool_output_projection_max_bytes) {
    throw std::invalid_argument("tool output projection byte limit is below the supported minimum");
  }
  if (policy.max_tokens < minimum_tool_output_projection_max_tokens) {
    throw std::invalid_argument(
      "tool output projection token limit is below the supported minimum");
  }
}

tool_output_projection_policy tighten_tool_output_projection_policy(
  tool_output_projection_policy base, const tool_output_projection_policy& ceiling) {
  validate_tool_output_projection_policy(base);
  validate_tool_output_projection_policy(ceiling);
  base.max_bytes = (std::min)(base.max_bytes, ceiling.max_bytes);
  base.max_tokens = (std::min)(base.max_tokens, ceiling.max_tokens);
  return base;
}

tool_output_projection_policy effective_tool_output_projection_policy(
  tool_output_projection_policy base, const tool_output_projection_constraints& constraints) {
  validate_tool_output_projection_policy(base);
  if (constraints.max_bytes) {
    base.max_bytes = (std::min)(base.max_bytes, *constraints.max_bytes);
  }
  if (constraints.max_tokens) {
    base.max_tokens = (std::min)(base.max_tokens, *constraints.max_tokens);
  }
  validate_tool_output_projection_policy(base);
  return base;
}

tool_output_projection_policy_validation check_tool_output_projection_policy(
  const tool_output_projection_policy& policy, const text_token_estimator& estimator) {
  try {
    validate_tool_output_projection_policy(policy);
  }
  catch (const std::invalid_argument& ex) {
    return policy_failure(tool_output_projection_error::invalid_policy, ex.what());
  }

  try {
    const auto maximum = (std::numeric_limits<std::size_t>::max)();
    const auto plain = plain_projection_envelope({}, maximum, maximum);

    tools::tool_outcome success;
    const auto structured_success = structured_projection_envelope(success, {}, maximum, maximum);

    if (!envelope_fits(plain, policy, estimator) ||
        !envelope_fits(structured_success, policy, estimator)) {
      return policy_failure(tool_output_projection_error::envelope_exceeds_limits,
        "tool output projection limits cannot contain every truncation envelope");
    }
    static constexpr std::array error_categories {
      tools::tool_error_category::invalid_input,
      tools::tool_error_category::not_found,
      tools::tool_error_category::permission_denied,
      tools::tool_error_category::conflict,
      tools::tool_error_category::rate_limited,
      tools::tool_error_category::timeout,
      tools::tool_error_category::cancelled,
      tools::tool_error_category::unavailable,
      tools::tool_error_category::internal,
    };
    for (const auto category : error_categories) {
      tools::tool_outcome failure {
        .error_code = std::error_code((std::numeric_limits<int>::min)(), std::generic_category()),
        .error_category = category,
        .retryable = false,
      };
      const auto envelope = structured_projection_envelope(failure, {}, maximum, maximum);
      if (!envelope_fits(envelope, policy, estimator)) {
        return policy_failure(tool_output_projection_error::envelope_exceeds_limits,
          "tool output projection limits cannot contain every truncation envelope");
      }
    }
  }
  catch (const std::bad_alloc&) {
    throw;
  }
  catch (const nlohmann::json::exception& ex) {
    return policy_failure(tool_output_projection_error::serialization_failure, ex.what());
  }
  catch (const std::exception& ex) {
    return policy_failure(tool_output_projection_error::estimator_failure, ex.what());
  }
  catch (...) {
    return policy_failure(
      tool_output_projection_error::estimator_failure, "tool output token estimator failed");
  }
  return {};
}

tool_output_projection_result try_project_tool_output_for_model(const tools::tool_outcome& result,
  const tool_output_projection_policy& policy, const text_token_estimator& estimator) {
  try {
    validate_tool_output_projection_policy(policy);
  }
  catch (const std::invalid_argument& ex) {
    return projection_failure(tool_output_projection_error::invalid_policy, ex.what());
  }

  try {
    auto canonical = canonical_tool_output_for_model(result);
    const auto original_bytes = canonical.size();
    const auto original_tokens = estimator.estimate_text(canonical);
    const bool byte_limited = original_bytes > policy.max_bytes;
    const bool token_limited = original_tokens > policy.max_tokens;
    if (!byte_limited && !token_limited) {
      return {
        .projection = {
          .content = std::move(canonical),
          .report = {
            .truncated = false,
            .original_bytes = original_bytes,
            .projected_bytes = original_bytes,
            .original_estimated_tokens = original_tokens,
            .projected_estimated_tokens = original_tokens,
            .max_bytes = policy.max_bytes,
            .max_tokens = policy.max_tokens,
          },
        },
      };
    }

    const auto build = [&](std::string_view preview) {
      return structured_tool_output(result)
               ? structured_projection_envelope(result, preview, original_bytes, original_tokens)
               : plain_projection_envelope(preview, original_bytes, original_tokens);
    };
    const auto empty_projection = build({});
    const auto empty_tokens = estimator.estimate_text(empty_projection);
    if (empty_projection.size() > policy.max_bytes || empty_tokens > policy.max_tokens) {
      return projection_failure(tool_output_projection_error::envelope_exceeds_limits,
        "tool output projection limits cannot contain the truncation envelope");
    }

    auto preview_bytes = policy.max_bytes - empty_projection.size();
    auto preview_tokens = policy.max_tokens - empty_tokens;
    std::string projected = empty_projection;
    for (std::size_t attempt = 0; attempt < preview_search_attempts; ++attempt) {
      const auto preview =
        bounded_middle_preview(canonical, preview_bytes, preview_tokens, estimator);
      projected = build(preview);
      const auto projected_tokens = estimator.estimate_text(projected);
      if (projected.size() <= policy.max_bytes && projected_tokens <= policy.max_tokens) {
        const auto projected_bytes = projected.size();
        return {
          .projection = {
            .content = std::move(projected),
            .report = {
              .truncated = true,
              .original_bytes = original_bytes,
              .projected_bytes = projected_bytes,
              .original_estimated_tokens = original_tokens,
              .projected_estimated_tokens = projected_tokens,
              .max_bytes = policy.max_bytes,
              .max_tokens = policy.max_tokens,
              .limiting_factor = limiting_factor(byte_limited, token_limited),
            },
          },
        };
      }
      if (preview_bytes == 0 && preview_tokens == 0) {
        break;
      }
      preview_bytes /= 2;
      preview_tokens /= 2;
    }

    return {
      .projection = {
        .content = empty_projection,
        .report = {
          .truncated = true,
          .original_bytes = original_bytes,
          .projected_bytes = empty_projection.size(),
          .original_estimated_tokens = original_tokens,
          .projected_estimated_tokens = empty_tokens,
          .max_bytes = policy.max_bytes,
          .max_tokens = policy.max_tokens,
          .limiting_factor = limiting_factor(byte_limited, token_limited),
        },
      },
    };
  }
  catch (const std::bad_alloc&) {
    throw;
  }
  catch (const nlohmann::json::exception& ex) {
    return projection_failure(tool_output_projection_error::serialization_failure, ex.what());
  }
  catch (const std::exception& ex) {
    return projection_failure(tool_output_projection_error::estimator_failure, ex.what());
  }
  catch (...) {
    return projection_failure(
      tool_output_projection_error::estimator_failure, "tool output token estimator failed");
  }
}

tool_output_projection project_tool_output_for_model(const tools::tool_outcome& result,
  const tool_output_projection_policy& policy, const text_token_estimator& estimator) {
  auto projected = try_project_tool_output_for_model(result, policy, estimator);
  if (projected) {
    return std::move(projected.projection);
  }
  if (projected.error == tool_output_projection_error::invalid_policy ||
      projected.error == tool_output_projection_error::envelope_exceeds_limits) {
    throw std::invalid_argument(projected.message);
  }
  throw std::runtime_error(
    projected.message.empty() ? "tool output projection failed" : std::move(projected.message));
}

} // namespace wuwe::agent::llm
