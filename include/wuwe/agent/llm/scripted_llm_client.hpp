#ifndef WUWE_AGENT_LLM_SCRIPTED_LLM_CLIENT_HPP
#define WUWE_AGENT_LLM_SCRIPTED_LLM_CLIENT_HPP

#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/llm/llm_client.h>

namespace wuwe::agent::llm {

struct scripted_llm_step {
  std::string label;
  std::function<bool(const llm_request&)> matches;
  llm_response response;
  std::vector<llm_stream_event> stream_events;
};

class scripted_llm_client final : public ::wuwe::llm_client {
public:
  scripted_llm_client() = default;

  explicit scripted_llm_client(std::vector<scripted_llm_step> steps)
      : steps_(steps.begin(), steps.end()) {
  }

  void push(scripted_llm_step step) {
    std::scoped_lock lock(mutex_);
    steps_.push_back(std::move(step));
  }

  [[nodiscard]] std::size_t remaining() const {
    std::scoped_lock lock(mutex_);
    return steps_.size();
  }

  [[nodiscard]] std::vector<llm_request> requests() const {
    std::scoped_lock lock(mutex_);
    return requests_;
  }

  llm_response complete(const llm_request& request) override {
    return complete(request, {});
  }

  llm_response complete(
    const llm_request& request,
    std::stop_token stop_token) override {
    if (stop_token.stop_requested()) {
      return cancelled();
    }
    auto step = consume(request);
    return step.response;
  }

  bool supports_streaming() const noexcept override {
    return true;
  }

  [[nodiscard]] llm_provider_capabilities capabilities()
    const noexcept override {
    return {
      .streaming = true,
      .tools = true,
      .tool_choice = true,
      .json_response_format = true,
      .stop_sequences = true,
      .deterministic_seed = true,
      .json_schema_output = true,
      .explicit_cache_control = true,
    };
  }

  llm_response complete_stream(
    const llm_request& request,
    const llm_stream_callbacks& callbacks,
    std::stop_token stop_token = {}) override {
    if (stop_token.stop_requested()) {
      return cancelled();
    }
    auto step = consume(request);
    for (const auto& event : step.stream_events) {
      if (stop_token.stop_requested()) {
        return cancelled();
      }
      emit(callbacks, event);
    }
    return step.response;
  }

private:
  scripted_llm_step consume(const llm_request& request) {
    std::scoped_lock consume_lock(consume_mutex_);
    scripted_llm_step candidate;
    {
      std::scoped_lock lock(mutex_);
      if (steps_.empty()) {
        throw std::logic_error("scripted LLM client has no remaining steps");
      }
      candidate = steps_.front();
    }
    if (candidate.matches && !candidate.matches(request)) {
      const auto suffix = candidate.label.empty()
        ? std::string {}
        : " '" + candidate.label + "'";
      throw std::logic_error("scripted LLM request did not match step" + suffix);
    }
    {
      std::scoped_lock lock(mutex_);
      requests_.push_back(request);
      auto output = std::move(steps_.front());
      steps_.pop_front();
      return output;
    }
  }

  static void emit(
    const llm_stream_callbacks& callbacks,
    const llm_stream_event& event) {
    if (callbacks.on_event) callbacks.on_event(event);
    if (event.type == llm_stream_event_type::reasoning_delta &&
        callbacks.on_reasoning_delta && !event.reasoning_delta.empty()) {
      callbacks.on_reasoning_delta(event.reasoning_delta);
    }
    if (event.type == llm_stream_event_type::reasoning_done &&
        callbacks.on_reasoning_done && !event.reasoning_summary.empty()) {
      callbacks.on_reasoning_done(event.reasoning_summary);
    }
  }

  static llm_response cancelled() {
    return {
      .content = "LLM request cancelled.",
      .error_code = agent::make_error_code(agent::llm_error_code::cancelled),
    };
  }

  mutable std::mutex mutex_;
  std::mutex consume_mutex_;
  std::deque<scripted_llm_step> steps_;
  std::vector<llm_request> requests_;
};

} // namespace wuwe::agent::llm

#endif // WUWE_AGENT_LLM_SCRIPTED_LLM_CLIENT_HPP
