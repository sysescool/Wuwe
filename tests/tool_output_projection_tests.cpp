#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <nlohmann/json.hpp>

#include <wuwe/agent/llm/context_token_estimator.hpp>
#include <wuwe/agent/llm/tool_output_projection.hpp>
#include <wuwe/agent/tools/tool_contract.hpp>
#include <wuwe/common/print.h>

namespace {

using namespace wuwe;
using namespace wuwe::agent;
using namespace wuwe::agent::llm;

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool valid_utf8(std::string_view text) {
  for (std::size_t index = 0; index < text.size();) {
    const auto first = static_cast<unsigned char>(text[index]);
    std::size_t continuation_bytes = 0;
    if (first < 0x80U) {
      ++index;
      continue;
    }
    if ((first & 0xe0U) == 0xc0U) {
      continuation_bytes = 1;
      if (first < 0xc2U) {
        return false;
      }
    }
    else if ((first & 0xf0U) == 0xe0U) {
      continuation_bytes = 2;
    }
    else if ((first & 0xf8U) == 0xf0U) {
      continuation_bytes = 3;
      if (first > 0xf4U) {
        return false;
      }
    }
    else {
      return false;
    }
    if (index + continuation_bytes >= text.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuation_bytes; ++offset) {
      if ((static_cast<unsigned char>(text[index + offset]) & 0xc0U) != 0x80U) {
        return false;
      }
    }
    if (continuation_bytes == 2) {
      const auto second = static_cast<unsigned char>(text[index + 1]);
      if ((first == 0xe0U && second < 0xa0U) || (first == 0xedU && second >= 0xa0U)) {
        return false;
      }
    }
    if (continuation_bytes == 3) {
      const auto second = static_cast<unsigned char>(text[index + 1]);
      if ((first == 0xf0U && second < 0x90U) || (first == 0xf4U && second >= 0x90U)) {
        return false;
      }
    }
    index += continuation_bytes + 1;
  }
  return true;
}

class byte_count_estimator : public text_token_estimator {
public:
  std::size_t estimate_text(std::string_view text) const override {
    return text.size();
  }

  std::string truncate_text(
    std::string_view text, std::size_t token_limit, bool keep_tail) const override {
    if (text.size() <= token_limit) {
      return std::string(text);
    }
    return keep_tail ? std::string(text.substr(text.size() - token_limit))
                     : std::string(text.substr(0, token_limit));
  }
};

class noncompliant_truncating_estimator final : public byte_count_estimator {
public:
  std::string truncate_text(std::string_view text, std::size_t, bool) const override {
    return std::string(text);
  }
};

class throwing_estimator final : public text_token_estimator {
public:
  std::size_t estimate_text(std::string_view) const override {
    throw std::runtime_error("estimator unavailable");
  }

  std::string truncate_text(std::string_view, std::size_t, bool) const override {
    throw std::runtime_error("estimator unavailable");
  }
};

void unchanged_projection_preserves_the_existing_model_payload() {
  heuristic_context_token_estimator estimator;
  tools::tool_outcome plain { .content = "unchanged output" };
  const auto projected = project_tool_output_for_model(plain, {}, estimator);
  require(projected.content == plain.content,
    "plain tool output below both limits must remain byte-for-byte unchanged");
  require(
    !projected.report.truncated && projected.report.original_bytes == projected.content.size() &&
      projected.report.projected_bytes == projected.content.size() &&
      projected.report.original_estimated_tokens == estimator.estimate_text(projected.content) &&
      projected.report.projected_estimated_tokens == estimator.estimate_text(projected.content),
    "an unchanged projection report must describe the exact payload");

  tools::tool_outcome structured {
    .content = "found",
    .data = { { "answer", 42 } },
    .artifacts = { "artifact://result" },
  };
  const auto structured_projection = project_tool_output_for_model(structured, {}, estimator);
  const auto expected = nlohmann::json({
                                         { "ok", true },
                                         { "message", "found" },
                                         { "data", { { "answer", 42 } } },
                                         { "artifacts", { "artifact://result" } },
                                       })
                          .dump();
  require(structured_projection.content == expected && !structured_projection.report.truncated,
    "structured output below the limits must preserve the established model payload");
}

void byte_token_and_combined_limits_are_independent() {
  heuristic_context_token_estimator estimator;

  const auto byte_limited = project_tool_output_for_model(
    { .content = std::string(2'000, 'b') }, { .max_bytes = 256, .max_tokens = 1'000 }, estimator);
  require(byte_limited.report.truncated &&
            byte_limited.report.limiting_factor == tool_output_projection_limit::bytes &&
            byte_limited.content.size() <= 256 &&
            estimator.estimate_text(byte_limited.content) <= 1'000,
    "the byte ceiling must apply without falsely reporting a token ceiling");

  std::string cjk;
  for (int index = 0; index < 300; ++index) {
    cjk += "界";
  }
  const auto token_limited = project_tool_output_for_model(
    { .content = cjk }, { .max_bytes = 4'096, .max_tokens = 64 }, estimator);
  require(token_limited.report.truncated &&
            token_limited.report.limiting_factor == tool_output_projection_limit::tokens &&
            token_limited.content.size() <= 4'096 &&
            estimator.estimate_text(token_limited.content) <= 64,
    "the estimated-token ceiling must apply independently of the byte ceiling");

  const auto combined = project_tool_output_for_model(
    { .content = std::string(5'000, 'x') }, { .max_bytes = 256, .max_tokens = 64 }, estimator);
  require(combined.report.truncated &&
            combined.report.limiting_factor == tool_output_projection_limit::bytes_and_tokens &&
            combined.content.size() <= 256 && estimator.estimate_text(combined.content) <= 64,
    "both ceilings must be enforced when both dimensions overflow");
}

void exact_boundaries_are_not_truncated() {
  heuristic_context_token_estimator estimator;
  const std::string content(256, 'x');
  const auto projected = project_tool_output_for_model(
    { .content = content }, { .max_bytes = content.size(), .max_tokens = 64 }, estimator);
  require(!projected.report.truncated && projected.content == content,
    "payloads exactly on both configured boundaries must not be truncated");
}

void final_limits_do_not_depend_on_estimator_truncation_quality() {
  byte_count_estimator exact_estimator;
  const tool_output_projection_policy policy { .max_bytes = 4'096, .max_tokens = 256 };
  const auto exact =
    project_tool_output_for_model({ .content = std::string(2'000, 'x') }, policy, exact_estimator);
  require(exact.report.truncated &&
            exact.report.limiting_factor == tool_output_projection_limit::tokens &&
            exact_estimator.estimate_text(exact.content) <= policy.max_tokens,
    "custom token estimators must participate in the final hard ceiling");

  noncompliant_truncating_estimator broken_estimator;
  const auto defensive =
    project_tool_output_for_model({ .content = std::string(2'000, 'x') }, policy, broken_estimator);
  require(defensive.report.truncated && defensive.content.size() <= policy.max_bytes &&
            broken_estimator.estimate_text(defensive.content) <= policy.max_tokens,
    "the final postcondition must hold even when a custom truncator returns oversized text");
}

void policy_preflight_and_nonthrowing_projection_report_explicit_failures() {
  heuristic_context_token_estimator heuristic;
  const auto supported =
    check_tool_output_projection_policy({ .max_bytes = 256, .max_tokens = 64 }, heuristic);
  require(static_cast<bool>(supported),
    "the supported minimum policy must contain every envelope under the fallback estimator");

  byte_count_estimator byte_estimator;
  const auto too_small_for_estimator =
    check_tool_output_projection_policy({ .max_bytes = 256, .max_tokens = 64 }, byte_estimator);
  require(!too_small_for_estimator &&
            too_small_for_estimator.error == tool_output_projection_error::envelope_exceeds_limits,
    "policy preflight must account for the injected estimator before a tool executes");

  throwing_estimator unavailable;
  const auto invalid_estimator = check_tool_output_projection_policy({}, unavailable);
  require(!invalid_estimator &&
            invalid_estimator.error == tool_output_projection_error::estimator_failure,
    "policy preflight must expose estimator failures without throwing");

  const auto projection =
    try_project_tool_output_for_model({ .content = std::string(1'000, 'x') }, {}, unavailable);
  require(!projection && projection.error == tool_output_projection_error::estimator_failure,
    "the nonthrowing projection API must return typed estimator failures");

  tools::tool_outcome invalid_utf8 {
    .content = std::string(1, static_cast<char>(0xff)),
    .data = { { "value", std::string(1'000, 'x') } },
  };
  const auto serialization_failure = try_project_tool_output_for_model(invalid_utf8, {}, heuristic);
  require(!serialization_failure &&
            serialization_failure.error == tool_output_projection_error::serialization_failure,
    "the nonthrowing projection API must classify structured serialization failures");
}

void truncation_is_utf8_safe_deterministic_and_keeps_context() {
  heuristic_context_token_estimator estimator;
  std::string content = "HEAD-你好🙂-";
  for (int index = 0; index < 400; ++index) {
    content += "数据🚀";
  }
  content += "-再见🙂-TAIL";
  const tool_output_projection_policy policy { .max_bytes = 256, .max_tokens = 1'000 };
  const auto first = project_tool_output_for_model({ .content = content }, policy, estimator);
  const auto second = project_tool_output_for_model({ .content = content }, policy, estimator);
  require(first.content == second.content,
    "the same payload, policy, and estimator must produce deterministic projections");
  require(valid_utf8(first.content), "projection must not split UTF-8 code points");
  require(first.content.find("HEAD-") != std::string::npos &&
            first.content.find("-TAIL") != std::string::npos &&
            first.content.find("tool output omitted") != std::string::npos,
    "a truncated preview should retain useful head and tail context with an omission marker");
  require(first.report.projected_bytes == first.content.size() &&
            first.report.projected_estimated_tokens == estimator.estimate_text(first.content) &&
            first.report.max_bytes == policy.max_bytes &&
            first.report.max_tokens == policy.max_tokens,
    "projection reports must match the exact emitted payload and effective policy");
}

void structured_envelopes_remain_valid_and_preserve_error_identity() {
  heuristic_context_token_estimator estimator;
  const tool_output_projection_policy policy { .max_bytes = 256, .max_tokens = 64 };

  tools::tool_outcome success {
    .content = "large structured success",
    .data = { { "payload",
      std::string(1'000, '"') + std::string(1'000, '\\') + std::string(1'000, '\n') } },
    .artifacts = { "artifact://large" },
  };
  const auto success_projection = project_tool_output_for_model(success, policy, estimator);
  const auto success_json = nlohmann::json::parse(success_projection.content);
  require(success_projection.report.truncated && success_json.at("ok").get<bool>() &&
            success_json.at("truncation").at("truncated").get<bool>() &&
            success_json.at("preview").is_string(),
    "truncated structured success output must remain a self-describing JSON object");

  tools::tool_outcome failure {
    .content = std::string(4'000, 'e'),
    .error_code = std::make_error_code(std::errc::timed_out),
    .error_category = tools::tool_error_category::timeout,
    .retryable = true,
  };
  const auto failure_projection = project_tool_output_for_model(failure, policy, estimator);
  const auto failure_json = nlohmann::json::parse(failure_projection.content);
  require(failure_projection.report.truncated && !failure_json.at("ok").get<bool>() &&
            failure_json.at("error").at("category") == "timeout" &&
            failure_json.at("error").at("code") == failure.error_code.value() &&
            failure_json.at("error").at("retryable").get<bool>(),
    "truncated errors must preserve their stable category, code, and retryability");
  require(failure_projection.content.size() <= policy.max_bytes &&
            estimator.estimate_text(failure_projection.content) <= policy.max_tokens,
    "structured truncation envelopes must obey the same hard ceilings as plain output");
}

void invalid_policies_and_descriptor_constraints_fail_explicitly() {
  bool rejected = false;
  try {
    validate_tool_output_projection_policy({ .max_bytes = 255, .max_tokens = 64 });
  }
  catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "projection byte limits below the supported envelope size must be rejected");

  rejected = false;
  try {
    validate_tool_output_projection_policy({ .max_bytes = 256, .max_tokens = 63 });
  }
  catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "projection token limits below the supported envelope size must be rejected");

  tools::tool_descriptor descriptor { .name = "bounded" };
  descriptor.model_output_projection.max_bytes = 255;
  rejected = false;
  try {
    tools::validate_tool_descriptor(descriptor);
  }
  catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "tool descriptors must reject unsupported projection constraints");

  descriptor.model_output_projection = { .max_bytes = 512, .max_tokens = 128 };
  const auto effective = effective_tool_output_projection_policy(
    { .max_bytes = 1'024, .max_tokens = 64 }, descriptor.model_output_projection);
  require(effective.max_bytes == 512 && effective.max_tokens == 64,
    "tool constraints may tighten but never widen runner-level limits");

  const auto serialized = tools::tool_descriptor_to_json(descriptor);
  require(serialized.at("schema_version") == 3 &&
            serialized.at("model_output_projection").at("max_bytes") == 512 &&
            serialized.at("model_output_projection").at("max_tokens") == 128,
    "descriptor serialization must publish the projection constraints explicitly");
}

void run(const char* name, void (*test)()) {
  test();
  println("[PASS] {}", name);
}

} // namespace

int main() {
  try {
    run("unchanged projection compatibility",
      unchanged_projection_preserves_the_existing_model_payload);
    run("independent projection ceilings", byte_token_and_combined_limits_are_independent);
    run("exact projection boundaries", exact_boundaries_are_not_truncated);
    run("defensive estimator enforcement",
      final_limits_do_not_depend_on_estimator_truncation_quality);
    run("projection failure API",
      policy_preflight_and_nonthrowing_projection_report_explicit_failures);
    run("UTF-8 deterministic projection", truncation_is_utf8_safe_deterministic_and_keeps_context);
    run("structured projection envelopes",
      structured_envelopes_remain_valid_and_preserve_error_identity);
    run(
      "projection policy validation", invalid_policies_and_descriptor_constraints_fail_explicitly);
  }
  catch (const std::exception& ex) {
    println("[FAIL] {}", ex.what());
    return 1;
  }
  return 0;
}
