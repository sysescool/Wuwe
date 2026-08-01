#ifndef WUWE_AGENT_LLM_CONTEXT_BUDGET_HPP
#define WUWE_AGENT_LLM_CONTEXT_BUDGET_HPP

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wuwe/agent/llm/llm_types.h>

namespace wuwe::agent::llm {

namespace detail {

inline void saturating_context_token_add(std::size_t& target, std::size_t value) noexcept {
  const auto maximum = (std::numeric_limits<std::size_t>::max)();
  target = value > maximum - target ? maximum : target + value;
}

[[nodiscard]] inline std::size_t saturating_context_token_sum(
  std::size_t lhs, std::size_t rhs) noexcept {
  saturating_context_token_add(lhs, rhs);
  return lhs;
}

} // namespace detail

class context_token_estimator {
public:
  virtual ~context_token_estimator() = default;
  [[nodiscard]] virtual std::size_t estimate_text(std::string_view text) const = 0;
  [[nodiscard]] virtual std::string truncate_text(
    std::string_view text, std::size_t token_limit, bool keep_tail) const = 0;

  [[nodiscard]] virtual std::size_t estimate_message(const ::wuwe::chat_message& message) const {
    std::size_t tokens = 4;
    detail::saturating_context_token_add(tokens, estimate_text(message.role));
    detail::saturating_context_token_add(tokens, estimate_text(message.content));
    if (message.name) {
      detail::saturating_context_token_add(tokens, estimate_text(*message.name));
    }
    if (message.tool_call_id) {
      detail::saturating_context_token_add(tokens, estimate_text(*message.tool_call_id));
    }
    for (const auto& call : message.tool_calls) {
      detail::saturating_context_token_add(tokens, 4);
      detail::saturating_context_token_add(tokens, estimate_text(call.id));
      detail::saturating_context_token_add(tokens, estimate_text(call.name));
      detail::saturating_context_token_add(tokens, estimate_text(call.arguments_json));
    }
    return tokens;
  }

  [[nodiscard]] virtual std::size_t estimate_tool(const ::wuwe::llm_tool& tool) const {
    std::size_t tokens = 8;
    detail::saturating_context_token_add(tokens, estimate_text(tool.name));
    detail::saturating_context_token_add(tokens, estimate_text(tool.description));
    detail::saturating_context_token_add(tokens, estimate_text(tool.parameters_json_schema));
    return tokens;
  }
};

class heuristic_context_token_estimator final : public context_token_estimator {
public:
  [[nodiscard]] std::size_t estimate_text(std::string_view text) const override {
    std::size_t ascii = 0;
    std::size_t non_ascii_codepoints = 0;
    for (std::size_t index = 0; index < text.size();) {
      const auto byte = static_cast<unsigned char>(text[index]);
      if (byte < 0x80) {
        ++ascii;
        ++index;
      }
      else {
        ++non_ascii_codepoints;
        if ((byte & 0xe0) == 0xc0)
          index += 2;
        else if ((byte & 0xf0) == 0xe0)
          index += 3;
        else if ((byte & 0xf8) == 0xf0)
          index += 4;
        else
          ++index;
        index = (std::min)(index, text.size());
      }
    }
    return (ascii + 3) / 4 + non_ascii_codepoints;
  }

  [[nodiscard]] std::string truncate_text(
    std::string_view text, std::size_t token_limit, bool keep_tail) const override {
    if (estimate_text(text) <= token_limit)
      return std::string(text);
    if (token_limit == 0)
      return {};
    std::size_t low = 0;
    std::size_t high = text.size();
    while (low < high) {
      const auto length = low + (high - low + 1) / 2;
      const auto candidate = keep_tail ? valid_tail(text, length) : valid_prefix(text, length);
      if (estimate_text(candidate) <= token_limit)
        low = length;
      else
        high = length - 1;
    }
    return std::string(keep_tail ? valid_tail(text, low) : valid_prefix(text, low));
  }

private:
  [[nodiscard]] static std::string_view valid_prefix(std::string_view text, std::size_t length) {
    length = (std::min)(length, text.size());
    while (length > 0 && length < text.size() &&
           (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) {
      --length;
    }
    return text.substr(0, length);
  }

  [[nodiscard]] static std::string_view valid_tail(std::string_view text, std::size_t length) {
    length = (std::min)(length, text.size());
    auto start = text.size() - length;
    while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) {
      ++start;
    }
    return text.substr(start);
  }
};

struct context_budget_usage {
  std::size_t system { 0 };
  std::size_t conversation { 0 };
  std::size_t memory { 0 };
  std::size_t knowledge { 0 };
  std::size_t skills { 0 };
  std::size_t tool_schemas { 0 };
  std::size_t tool_results { 0 };
  std::size_t other { 0 };
  std::size_t reserved_output { 0 };

  [[nodiscard]] std::size_t input_total() const noexcept {
    std::size_t total = system;
    for (const auto value :
      { conversation, memory, knowledge, skills, tool_schemas, tool_results, other }) {
      detail::saturating_context_token_add(total, value);
    }
    return total;
  }
};

struct context_budget_report {
  context_budget_usage before;
  context_budget_usage after;
  std::size_t dropped_messages { 0 };
  std::size_t truncated_messages { 0 };
  bool fitted { false };
  std::string error;
};

struct context_budget_result {
  ::wuwe::llm_request request;
  context_budget_report report;
  explicit operator bool() const noexcept {
    return report.fitted;
  }
};

[[nodiscard]] inline llm_context_source resolved_context_source(
  const ::wuwe::chat_message& message) {
  if (message.context_source != llm_context_source::automatic) {
    return message.context_source;
  }
  if (message.role == "system")
    return llm_context_source::system;
  if (message.role == "tool" || message.tool_call_id || !message.tool_calls.empty()) {
    return llm_context_source::tool_result;
  }
  return llm_context_source::conversation;
}

class context_budget_manager {
public:
  explicit context_budget_manager(std::shared_ptr<const context_token_estimator> estimator = {})
      : estimator_(estimator ? std::move(estimator)
                             : std::make_shared<heuristic_context_token_estimator>()) {
  }

  [[nodiscard]] context_budget_result fit(
    ::wuwe::llm_request request, const ::wuwe::llm_context_budget& budget) const {
    context_budget_result result { .request = std::move(request) };
    if (budget.context_window_tokens == 0) {
      result.report.error = "context window must be greater than zero";
      return result;
    }
    if (result.request.max_output_tokens && *result.request.max_output_tokens <= 0) {
      result.report.error = "max output tokens must be positive when specified";
      return result;
    }
    const auto requested_output =
      result.request.max_output_tokens && *result.request.max_output_tokens > 0
        ? static_cast<std::size_t>(*result.request.max_output_tokens)
        : std::size_t { 0 };
    const auto reserved_output = (std::max)(budget.reserved_output_tokens, requested_output);
    if (reserved_output >= budget.context_window_tokens) {
      result.report.error = "reserved output consumes the entire context window";
      return result;
    }
    if (!result.request.max_output_tokens && reserved_output != 0) {
      if (reserved_output > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        result.report.error = "reserved output exceeds the supported token range";
        return result;
      }
      result.request.max_output_tokens = static_cast<int>(reserved_output);
    }

    std::vector<bool> removed(result.request.messages.size(), false);
    result.report.before = usage(result.request, removed, reserved_output);
    result.report.after = result.report.before;
    const auto input_limit = budget.context_window_tokens - reserved_output;
    if (budget.limits.tool_schemas != 0 &&
        result.report.before.tool_schemas > budget.limits.tool_schemas) {
      result.report.error = "tool schemas exceed their context component limit";
      return result;
    }

    if (!apply_declared_limits(result, removed, budget))
      return result;
    result.report.after = usage(result.request, removed, reserved_output);
    const auto immutable_tokens =
      detail::saturating_context_token_sum(result.report.after.tool_schemas,
        budget.allow_system_truncation ? 0 : result.report.after.system);
    if (immutable_tokens > input_limit) {
      result.report.error = "protected system context and tool schemas exceed the input budget";
      return result;
    }
    if (result.report.after.input_total() > input_limit &&
        budget.overflow == llm_context_overflow_policy::reject) {
      result.report.error = "input exceeds the available context window";
      return result;
    }

    static constexpr llm_context_source reduction_order[] {
      llm_context_source::memory,
      llm_context_source::knowledge,
      llm_context_source::skill,
      llm_context_source::tool_result,
      llm_context_source::conversation,
      llm_context_source::other,
    };
    for (const auto source : reduction_order) {
      if (result.report.after.input_total() <= input_limit)
        break;
      const auto current = component(result.report.after, source);
      const auto excess = result.report.after.input_total() - input_limit;
      const auto target = current > excess ? current - excess : 0;
      const auto preserve = source == llm_context_source::conversation
                              ? budget.minimum_recent_conversation_messages
                              : 0;
      (void)reduce(result, removed, source, target, true, preserve);
      result.report.after = usage(result.request, removed, reserved_output);
    }

    if (result.report.after.input_total() > input_limit && budget.allow_system_truncation) {
      const auto excess = result.report.after.input_total() - input_limit;
      const auto target =
        result.report.after.system > excess ? result.report.after.system - excess : 0;
      (void)reduce(result, removed, llm_context_source::system, target, true, 0);
      result.report.after = usage(result.request, removed, reserved_output);
    }
    if (result.report.after.input_total() > input_limit) {
      result.report.error = "protected context exceeds the available input budget";
      return result;
    }

    std::vector<::wuwe::chat_message> fitted;
    fitted.reserve(result.request.messages.size() - result.report.dropped_messages);
    for (std::size_t index = 0; index < result.request.messages.size(); ++index) {
      if (!removed[index])
        fitted.push_back(std::move(result.request.messages[index]));
    }
    result.request.messages = std::move(fitted);
    result.report.fitted = true;
    return result;
  }

private:
  [[nodiscard]] bool apply_declared_limits(context_budget_result& result,
    std::vector<bool>& removed, const ::wuwe::llm_context_budget& budget) const {
    const struct limit_entry {
      llm_context_source source;
      std::size_t limit;
      bool truncate;
      std::size_t preserve;
    } entries[] {
      { llm_context_source::system, budget.limits.system, budget.allow_system_truncation, 0 },
      { llm_context_source::memory, budget.limits.memory, true, 0 },
      { llm_context_source::knowledge, budget.limits.knowledge, true, 0 },
      { llm_context_source::skill, budget.limits.skills, true, 0 },
      { llm_context_source::tool_result, budget.limits.tool_results, true, 0 },
      { llm_context_source::conversation,
        budget.limits.conversation,
        true,
        budget.minimum_recent_conversation_messages },
      { llm_context_source::other, budget.limits.other, true, 0 },
    };
    for (const auto& entry : entries) {
      if (entry.limit == 0)
        continue;
      if (!reduce(result, removed, entry.source, entry.limit, entry.truncate, entry.preserve)) {
        if (result.report.error.empty()) {
          result.report.error = "a context component cannot be reduced to its limit";
        }
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] context_budget_usage usage(const ::wuwe::llm_request& request,
    const std::vector<bool>& removed, std::size_t reserved_output) const {
    context_budget_usage output { .reserved_output = reserved_output };
    for (std::size_t index = 0; index < request.messages.size(); ++index) {
      if (index < removed.size() && removed[index])
        continue;
      detail::saturating_context_token_add(
        component(output, resolved_context_source(request.messages[index])),
        estimator_->estimate_message(request.messages[index]));
    }
    for (const auto& tool : request.tools) {
      detail::saturating_context_token_add(output.tool_schemas, estimator_->estimate_tool(tool));
    }
    return output;
  }

  [[nodiscard]] bool reduce(context_budget_result& result, std::vector<bool>& removed,
    llm_context_source source, std::size_t limit, bool may_truncate,
    std::size_t preserve_recent) const {
    auto current =
      component(usage(result.request, removed, result.report.before.reserved_output), source);
    if (current <= limit)
      return true;
    if (!may_truncate) {
      result.report.error = "protected context component exceeds its limit";
      return false;
    }

    std::vector<std::size_t> candidates;
    for (std::size_t index = 0; index < result.request.messages.size(); ++index) {
      if (!removed[index] && resolved_context_source(result.request.messages[index]) == source) {
        candidates.push_back(index);
      }
    }
    const auto removable =
      candidates.size() > preserve_recent ? candidates.size() - preserve_recent : 0;
    for (std::size_t position = 0; position < candidates.size() && current > limit; ++position) {
      const auto index = candidates[position];
      if (removed[index]) {
        continue;
      }
      auto& message = result.request.messages[index];
      const auto tokens = estimator_->estimate_message(message);
      const bool protected_recent = position >= removable;
      if (protected_recent) {
        continue;
      }
      if (source == llm_context_source::tool_result && !message.tool_calls.empty()) {
        remove_tool_exchange(result, removed, index);
      }
      else if (current - (std::min)(current, tokens) >= limit || !can_truncate(message)) {
        removed[index] = true;
        ++result.report.dropped_messages;
      }
      else if (can_truncate(message)) {
        const auto excess = current - limit;
        truncate_message(message, excess);
        ++result.report.truncated_messages;
      }
      current =
        component(usage(result.request, removed, result.report.before.reserved_output), source);
    }
    return current <= limit;
  }

  static void remove_tool_exchange(
    context_budget_result& result, std::vector<bool>& removed, std::size_t assistant_index) {
    std::vector<std::string> call_ids;
    for (const auto& call : result.request.messages[assistant_index].tool_calls) {
      call_ids.push_back(call.id);
    }
    const auto remove = [&](std::size_t index) {
      if (!removed[index]) {
        removed[index] = true;
        ++result.report.dropped_messages;
      }
    };
    remove(assistant_index);
    for (std::size_t index = assistant_index + 1; index < result.request.messages.size(); ++index) {
      const auto& message = result.request.messages[index];
      if (resolved_context_source(message) != llm_context_source::tool_result) {
        break;
      }
      if (!message.tool_call_id ||
          std::find(call_ids.begin(), call_ids.end(), *message.tool_call_id) == call_ids.end()) {
        break;
      }
      remove(index);
    }
  }

  [[nodiscard]] static bool can_truncate(const ::wuwe::chat_message& message) {
    return message.tool_calls.empty() && !message.content.empty();
  }

  void truncate_message(::wuwe::chat_message& message, std::size_t tokens_to_remove) const {
    const auto content_tokens = estimator_->estimate_text(message.content);
    const auto target = content_tokens > tokens_to_remove ? content_tokens - tokens_to_remove : 0;
    const bool keep_tail = resolved_context_source(message) == llm_context_source::conversation;
    message.content = estimator_->truncate_text(message.content, target, keep_tail);
  }

  [[nodiscard]] static std::size_t component(
    const context_budget_usage& usage, llm_context_source source) {
    switch (source) {
      case llm_context_source::system:
        return usage.system;
      case llm_context_source::conversation:
        return usage.conversation;
      case llm_context_source::memory:
        return usage.memory;
      case llm_context_source::knowledge:
        return usage.knowledge;
      case llm_context_source::skill:
        return usage.skills;
      case llm_context_source::tool_result:
        return usage.tool_results;
      case llm_context_source::other:
      case llm_context_source::automatic:
        return usage.other;
    }
    return usage.other;
  }

  [[nodiscard]] static std::size_t& component(
    context_budget_usage& usage, llm_context_source source) {
    switch (source) {
      case llm_context_source::system:
        return usage.system;
      case llm_context_source::conversation:
        return usage.conversation;
      case llm_context_source::memory:
        return usage.memory;
      case llm_context_source::knowledge:
        return usage.knowledge;
      case llm_context_source::skill:
        return usage.skills;
      case llm_context_source::tool_result:
        return usage.tool_results;
      case llm_context_source::other:
      case llm_context_source::automatic:
        return usage.other;
    }
    return usage.other;
  }

  std::shared_ptr<const context_token_estimator> estimator_;
};

} // namespace wuwe::agent::llm

#endif // WUWE_AGENT_LLM_CONTEXT_BUDGET_HPP
