#ifndef WUWE_AGENT_LLM_RESILIENT_LLM_CLIENT_HPP
#define WUWE_AGENT_LLM_RESILIENT_LLM_CLIENT_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <wuwe/agent/llm/llm_client.h>
#include <wuwe/agent/llm/llm_usage.hpp>

namespace wuwe::agent::llm {

struct llm_retry_policy {
  std::size_t max_retries { 0 };
  std::chrono::milliseconds initial_backoff { 500 };
  std::chrono::milliseconds max_backoff { 30000 };
  std::chrono::milliseconds max_server_delay { 60000 };
  double jitter_ratio { 0.2 };
  bool respect_retry_after { true };
};

struct llm_rate_limit_policy {
  std::size_t max_requests { 0 };
  std::chrono::milliseconds window { 1000 };
  std::chrono::milliseconds max_wait { 0 };
};

struct llm_circuit_breaker_policy {
  std::size_t failure_threshold { 5 };
  std::chrono::milliseconds open_duration { 30000 };
};

enum class llm_resilience_event_type {
  attempt_started,
  retry_scheduled,
  fallback_selected,
  circuit_opened,
  circuit_rejected,
  rate_limit_rejected,
  completed,
};

struct llm_resilience_event {
  llm_resilience_event_type type { llm_resilience_event_type::attempt_started };
  std::string backend_id;
  std::size_t backend_index {};
  std::size_t attempt {};
  std::chrono::milliseconds delay {};
  std::error_code error_code;
};

struct llm_resilient_backend {
  std::string id;
  std::shared_ptr<::wuwe::llm_client> client;
  std::string model_override;
};

struct resilient_llm_client_options {
  llm_retry_policy retry;
  llm_rate_limit_policy rate_limit;
  llm_circuit_breaker_policy circuit_breaker;
  bool allow_fallback { true };
  std::function<double()> random_unit;
  std::function<void(const llm_resilience_event&)> observer;
};

class resilient_llm_client final : public ::wuwe::llm_client {
public:
  resilient_llm_client(
    std::vector<llm_resilient_backend> backends,
    resilient_llm_client_options options = {})
      : backends_(std::move(backends)),
        options_(std::move(options)),
        states_(backends_.size()) {
    if (backends_.empty()) {
      throw std::invalid_argument("resilient_llm_client requires at least one backend");
    }
    for (std::size_t index = 0; index < backends_.size(); ++index) {
      if (!backends_[index].client) {
        throw std::invalid_argument("resilient_llm_client backend requires a client");
      }
      if (backends_[index].id.empty()) {
        backends_[index].id = "backend-" + std::to_string(index + 1);
      }
    }
    validate_options();
  }

  llm_response complete(const llm_request& request) override {
    return complete(request, {});
  }

  llm_response complete(
    const llm_request& request,
    std::stop_token stop_token) override {
    return execute(request, stop_token, nullptr);
  }

  bool supports_streaming() const noexcept override {
    return std::any_of(backends_.begin(), backends_.end(), [](const auto& backend) {
      return backend.client->supports_streaming();
    });
  }

  [[nodiscard]] llm_provider_capabilities capabilities()
    const noexcept override {
    auto result = backends_.front().client->capabilities();
    for (std::size_t index = 1; index < backends_.size(); ++index) {
      const auto current = backends_[index].client->capabilities();
      result.declared = result.declared && current.declared;
      result.streaming = result.streaming && current.streaming;
      result.tools = result.tools && current.tools;
      result.tool_choice = result.tool_choice && current.tool_choice;
      result.json_response_format =
        result.json_response_format && current.json_response_format;
      result.reasoning_summary =
        result.reasoning_summary && current.reasoning_summary;
      result.streaming_reasoning_summary =
        result.streaming_reasoning_summary &&
        current.streaming_reasoning_summary;
      if (result.reasoning_language_control !=
          current.reasoning_language_control) {
        result.reasoning_language_control =
          llm_reasoning_language_control::unsupported;
      }
      result.multimodal_input =
        result.multimodal_input && current.multimodal_input;
      result.local_runtime = result.local_runtime && current.local_runtime;
      result.stop_sequences = result.stop_sequences && current.stop_sequences;
      result.deterministic_seed =
        result.deterministic_seed && current.deterministic_seed;
      result.json_schema_output =
        result.json_schema_output && current.json_schema_output;
      result.explicit_cache_control =
        result.explicit_cache_control && current.explicit_cache_control;
    }
    return result;
  }

  llm_response complete_stream(
    const llm_request& request,
    const llm_stream_callbacks& callbacks,
    std::stop_token stop_token = {}) override {
    return execute(request, stop_token, &callbacks);
  }

private:
  enum class circuit_permission {
    permitted,
    open,
    probe_in_progress,
  };

  struct backend_state {
    std::mutex mutex;
    std::deque<std::chrono::steady_clock::time_point> request_times;
    std::size_t consecutive_failures {};
    std::chrono::steady_clock::time_point open_until {};
    bool half_open_probe_in_progress { false };

    backend_state() = default;
    backend_state(const backend_state&) = delete;
    backend_state& operator=(const backend_state&) = delete;
  };

  static bool retryable(const std::error_code& error) noexcept {
    using agent::llm_error_code;
    return error == llm_error_code::rate_limited ||
           error == llm_error_code::timeout ||
           error == llm_error_code::transport_error ||
           error == llm_error_code::http_error ||
           error == llm_error_code::api_error ||
           error == llm_error_code::model_unavailable ||
           error == llm_error_code::circuit_open ||
           error == llm_error_code::rate_limit_wait_exceeded;
  }

  void validate_options() const {
    if (options_.retry.initial_backoff.count() < 0 ||
        options_.retry.max_backoff.count() < 0 ||
        options_.retry.initial_backoff > options_.retry.max_backoff ||
        options_.retry.max_server_delay.count() < 0 ||
        !std::isfinite(options_.retry.jitter_ratio) ||
        options_.retry.jitter_ratio < 0.0 ||
        options_.retry.jitter_ratio > 1.0) {
      throw std::invalid_argument("invalid LLM retry policy");
    }
    if (options_.rate_limit.window.count() <= 0 ||
        options_.rate_limit.max_wait.count() < 0) {
      throw std::invalid_argument("invalid LLM rate-limit policy");
    }
    if (options_.circuit_breaker.open_duration.count() < 0) {
      throw std::invalid_argument("invalid LLM circuit-breaker policy");
    }
  }

  llm_response execute(
    const llm_request& original_request,
    std::stop_token stop_token,
    const llm_stream_callbacks* stream_callbacks) {
    if (auto rejected = llm_request_rejection(
          original_request, capabilities())) {
      if (stream_callbacks) {
        emit_llm_request_rejection(*stream_callbacks, *rejected);
      }
      return std::move(*rejected);
    }
    llm_response last_response;
    llm_usage accumulated_usage;
    const auto cancelled_with_usage = [&] {
      auto response = cancelled_response();
      response.usage = accumulated_usage;
      return response;
    };
    bool attempted = false;
    for (std::size_t backend_index = 0;
         backend_index < backends_.size();
         ++backend_index) {
      if (backend_index != 0 && !options_.allow_fallback) {
        break;
      }
      if (stop_token.stop_requested()) {
        return cancelled_with_usage();
      }
      if (backend_index != 0) {
        publish({
          .type = llm_resilience_event_type::fallback_selected,
          .backend_id = backends_[backend_index].id,
          .backend_index = backend_index,
        });
      }

      for (std::size_t attempt = 0;
           attempt <= options_.retry.max_retries;
           ++attempt) {
        const auto permission = acquire_circuit_permission(backend_index);
        if (permission != circuit_permission::permitted) {
          last_response = {
            .content = permission == circuit_permission::open
              ? "LLM provider circuit is open."
              : "LLM provider circuit probe is already running.",
            .error_code = agent::make_error_code(
              agent::llm_error_code::circuit_open),
          };
          last_response.metadata["backend_id"] = backends_[backend_index].id;
          publish({
            .type = llm_resilience_event_type::circuit_rejected,
            .backend_id = backends_[backend_index].id,
            .backend_index = backend_index,
            .attempt = attempt,
            .error_code = last_response.error_code,
          });
          break;
        }

        if (!acquire_rate_limit(backend_index, stop_token)) {
          abandon_half_open_probe(backend_index);
          if (stop_token.stop_requested()) {
            return cancelled_with_usage();
          }
          last_response = {
            .content = "LLM provider rate-limit wait exceeded.",
            .error_code = agent::make_error_code(
              agent::llm_error_code::rate_limit_wait_exceeded),
          };
          last_response.metadata["backend_id"] = backends_[backend_index].id;
          publish({
            .type = llm_resilience_event_type::rate_limit_rejected,
            .backend_id = backends_[backend_index].id,
            .backend_index = backend_index,
            .attempt = attempt,
            .error_code = last_response.error_code,
          });
          break;
        }

        attempted = true;
        publish({
          .type = llm_resilience_event_type::attempt_started,
          .backend_id = backends_[backend_index].id,
          .backend_index = backend_index,
          .attempt = attempt,
        });
        auto request = original_request;
        if (!backends_[backend_index].model_override.empty()) {
          request.model = backends_[backend_index].model_override;
        }
        bool emitted_output = false;
        std::exception_ptr callback_failure;
        try {
          if (stream_callbacks) {
            auto proxy = make_stream_proxy(
              *stream_callbacks, emitted_output, callback_failure);
            last_response = backends_[backend_index].client->complete_stream(
              request, proxy, stop_token);
            if (callback_failure) {
              std::rethrow_exception(callback_failure);
            }
          }
          else {
            last_response = backends_[backend_index].client->complete(
              request, stop_token);
          }
        }
        catch (const std::exception& error) {
          abandon_half_open_probe(backend_index);
          if (callback_failure) {
            std::rethrow_exception(callback_failure);
          }
          last_response = {
            .content = error.what(),
            .error_code = agent::make_error_code(
              agent::llm_error_code::transport_error),
          };
          last_response.metadata["backend_exception"] = "true";
        }
        catch (...) {
          abandon_half_open_probe(backend_index);
          if (callback_failure) {
            std::rethrow_exception(callback_failure);
          }
          last_response = {
            .content = "LLM backend threw a non-standard exception.",
            .error_code = agent::make_error_code(
              agent::llm_error_code::transport_error),
          };
          last_response.metadata["backend_exception"] = "true";
        }
        accumulate_llm_usage(accumulated_usage, last_response.usage);
        last_response.usage = accumulated_usage;
        last_response.metadata["backend_id"] = backends_[backend_index].id;
        last_response.metadata["resilience_attempt"] = std::to_string(attempt + 1);
        last_response.metadata["fallback_index"] = std::to_string(backend_index);

        if (!last_response.error_code) {
          record_success(backend_index);
          publish({
            .type = llm_resilience_event_type::completed,
            .backend_id = backends_[backend_index].id,
            .backend_index = backend_index,
            .attempt = attempt,
          });
          return last_response;
        }
        if (!retryable(last_response.error_code)) {
          record_success(backend_index);
          forward_final_stream_error(stream_callbacks, last_response, emitted_output);
          return last_response;
        }

        const bool circuit_opened = record_failure(backend_index);
        if (emitted_output) {
          forward_final_stream_error(stream_callbacks, last_response, true);
          return last_response;
        }
        if (attempt >= options_.retry.max_retries || circuit_opened) {
          break;
        }
        const auto delay = retry_delay(attempt, last_response);
        publish({
          .type = llm_resilience_event_type::retry_scheduled,
          .backend_id = backends_[backend_index].id,
          .backend_index = backend_index,
          .attempt = attempt,
          .delay = delay,
          .error_code = last_response.error_code,
        });
        if (!interruptible_wait(stop_token, delay)) {
          return cancelled_with_usage();
        }
      }
    }

    if (!attempted && !last_response.error_code) {
      last_response = {
        .content = "No LLM provider backend was available.",
        .error_code = agent::make_error_code(agent::llm_error_code::circuit_open),
      };
    }
    last_response.metadata["fallback_exhausted"] = "true";
    last_response.usage = accumulated_usage;
    forward_final_stream_error(stream_callbacks, last_response, false);
    return last_response;
  }

  llm_stream_callbacks make_stream_proxy(
    const llm_stream_callbacks& destination,
    bool& emitted_output,
    std::exception_ptr& callback_failure) const {
    llm_stream_callbacks proxy;
    proxy.on_event = [&](const llm_stream_event& event) {
      if (event.type == llm_stream_event_type::error) {
        return;
      }
      if (event.type == llm_stream_event_type::content_delta ||
          event.type == llm_stream_event_type::reasoning_delta ||
          event.type == llm_stream_event_type::reasoning_done ||
          event.type == llm_stream_event_type::tool_call_delta ||
          event.type == llm_stream_event_type::tool_call_done) {
        emitted_output = true;
      }
      if (destination.on_event && !callback_failure) {
        try {
          destination.on_event(event);
        }
        catch (...) {
          callback_failure = std::current_exception();
        }
      }
    };
    proxy.on_reasoning_delta = [&](std::string_view delta) {
      if (!destination.on_reasoning_delta || callback_failure) return;
      try {
        destination.on_reasoning_delta(delta);
      }
      catch (...) {
        callback_failure = std::current_exception();
      }
    };
    proxy.on_reasoning_done = [&](std::string_view summary) {
      if (!destination.on_reasoning_done || callback_failure) return;
      try {
        destination.on_reasoning_done(summary);
      }
      catch (...) {
        callback_failure = std::current_exception();
      }
    };
    return proxy;
  }

  static void forward_final_stream_error(
    const llm_stream_callbacks* callbacks,
    const llm_response& response,
    bool) {
    if (!callbacks || !response.error_code || !callbacks->on_event) {
      return;
    }
    callbacks->on_event({
      .type = llm_stream_event_type::error,
      .response = response,
      .error_code = response.error_code,
      .message = response.content,
    });
  }

  circuit_permission acquire_circuit_permission(std::size_t index) {
    if (options_.circuit_breaker.failure_threshold == 0) {
      return circuit_permission::permitted;
    }
    auto& state = states_[index];
    std::scoped_lock lock(state.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (state.open_until == std::chrono::steady_clock::time_point {}) {
      return circuit_permission::permitted;
    }
    if (now < state.open_until) {
      return circuit_permission::open;
    }
    if (state.half_open_probe_in_progress) {
      return circuit_permission::probe_in_progress;
    }
    state.half_open_probe_in_progress = true;
    return circuit_permission::permitted;
  }

  void abandon_half_open_probe(std::size_t index) {
    auto& state = states_[index];
    std::scoped_lock lock(state.mutex);
    state.half_open_probe_in_progress = false;
  }

  void record_success(std::size_t index) {
    auto& state = states_[index];
    std::scoped_lock lock(state.mutex);
    state.consecutive_failures = 0;
    state.open_until = {};
    state.half_open_probe_in_progress = false;
  }

  bool record_failure(std::size_t index) {
    if (options_.circuit_breaker.failure_threshold == 0) {
      return false;
    }
    auto& state = states_[index];
    bool opened = false;
    {
      std::scoped_lock lock(state.mutex);
      state.half_open_probe_in_progress = false;
      ++state.consecutive_failures;
      if (state.consecutive_failures >=
          options_.circuit_breaker.failure_threshold) {
        state.open_until = std::chrono::steady_clock::now() +
          options_.circuit_breaker.open_duration;
        opened = true;
      }
    }
    if (opened) {
      publish({
        .type = llm_resilience_event_type::circuit_opened,
        .backend_id = backends_[index].id,
        .backend_index = index,
      });
    }
    return opened;
  }

  bool acquire_rate_limit(std::size_t index, std::stop_token stop_token) {
    if (options_.rate_limit.max_requests == 0) {
      return true;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + options_.rate_limit.max_wait;
    for (;;) {
      auto& state = states_[index];
      std::chrono::milliseconds wait_for {};
      {
        std::scoped_lock lock(state.mutex);
        const auto now = std::chrono::steady_clock::now();
        while (!state.request_times.empty() &&
               now - state.request_times.front() >= options_.rate_limit.window) {
          state.request_times.pop_front();
        }
        if (state.request_times.size() < options_.rate_limit.max_requests) {
          state.request_times.push_back(now);
          return true;
        }
        wait_for = std::chrono::duration_cast<std::chrono::milliseconds>(
          state.request_times.front() + options_.rate_limit.window - now);
      }
      if (options_.rate_limit.max_wait == std::chrono::milliseconds::zero() ||
          std::chrono::steady_clock::now() + wait_for > deadline) {
        return false;
      }
      if (!interruptible_wait(stop_token, wait_for)) {
        return false;
      }
    }
  }

  std::chrono::milliseconds retry_delay(
    std::size_t attempt,
    const llm_response& response) const {
    const auto exponent = (std::min<std::size_t>)(attempt, 20);
    const auto base_count = (std::max<std::int64_t>)(
      0, options_.retry.initial_backoff.count());
    const auto maximum = (std::max)(
      options_.retry.initial_backoff, options_.retry.max_backoff);
    const auto multiplier = std::int64_t { 1 } << exponent;
    const auto maximum_count = (std::max<std::int64_t>)(0, maximum.count());
    const auto scaled = base_count > maximum_count / multiplier
      ? maximum_count
      : (std::min)(base_count * multiplier, maximum_count);
    double delay = static_cast<double>(scaled);
    if (options_.retry.jitter_ratio > 0.0) {
      const auto unit = random_unit();
      delay *= 1.0 + ((std::clamp)(unit, 0.0, 1.0) * 2.0 - 1.0) *
        options_.retry.jitter_ratio;
    }
    delay = (std::max)(0.0, delay);
    const auto result_before_server = delay >= static_cast<double>(maximum_count)
      ? std::chrono::milliseconds(maximum_count)
      : std::chrono::milliseconds(static_cast<std::int64_t>(delay));
    auto result = result_before_server;
    if (options_.retry.respect_retry_after) {
      const auto found = response.metadata.find("retry_after_ms");
      if (found != response.metadata.end()) {
        try {
          const auto server_delay = std::chrono::milliseconds(
            std::stoll(found->second));
          result = (std::max)(result, (std::min)(
            server_delay, options_.retry.max_server_delay));
        }
        catch (...) {
        }
      }
    }
    return result;
  }

  double random_unit() const noexcept {
    if (options_.random_unit) {
      try {
        const auto value = options_.random_unit();
        return std::isfinite(value) ? (std::clamp)(value, 0.0, 1.0) : 0.5;
      }
      catch (...) {
        return 0.5;
      }
    }
    try {
      thread_local std::mt19937_64 source(std::random_device {}());
      thread_local std::uniform_real_distribution<double> distribution(0.0, 1.0);
      const auto value = distribution(source);
      return std::isfinite(value) ? (std::clamp)(value, 0.0, 1.0) : 0.5;
    }
    catch (...) {
      return 0.5;
    }
  }

  static bool interruptible_wait(
    std::stop_token stop_token,
    std::chrono::milliseconds duration) {
    constexpr auto quantum = std::chrono::milliseconds(25);
    while (duration > std::chrono::milliseconds::zero()) {
      if (stop_token.stop_requested()) {
        return false;
      }
      const auto wait = (std::min)(duration, quantum);
      std::this_thread::sleep_for(wait);
      duration -= wait;
    }
    return !stop_token.stop_requested();
  }

  static llm_response cancelled_response() {
    return {
      .content = "LLM request cancelled.",
      .error_code = agent::make_error_code(agent::llm_error_code::cancelled),
    };
  }

  void publish(llm_resilience_event event) const noexcept {
    if (!options_.observer) {
      return;
    }
    try {
      options_.observer(event);
    }
    catch (...) {
    }
  }

  std::vector<llm_resilient_backend> backends_;
  resilient_llm_client_options options_;
  std::vector<backend_state> states_;
};

} // namespace wuwe::agent::llm

#endif // WUWE_AGENT_LLM_RESILIENT_LLM_CLIENT_HPP
