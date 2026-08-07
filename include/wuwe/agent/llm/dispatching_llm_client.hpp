#ifndef WUWE_AGENT_LLM_DISPATCHING_LLM_CLIENT_HPP
#define WUWE_AGENT_LLM_DISPATCHING_LLM_CLIENT_HPP

#include <algorithm>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wuwe/agent/llm/detail/llm_request_runtime_context.hpp>
#include <wuwe/agent/llm/llm_capabilities.hpp>
#include <wuwe/agent/llm/llm_client_registry.hpp>

namespace wuwe::agent::llm {

namespace detail {

struct llm_dispatch_identity {};

struct llm_dispatch_lineage {
  llm_dispatch_lineage(std::shared_ptr<const llm_dispatch_identity> dispatcher_value,
    std::shared_ptr<const llm_dispatch_lineage> parent_value)
      : dispatcher(std::move(dispatcher_value)), parent(std::move(parent_value)) {
  }

  std::shared_ptr<const llm_dispatch_identity> dispatcher;
  std::shared_ptr<const llm_dispatch_lineage> parent;
};

} // namespace detail

enum class llm_dispatch_event_type {
  client_selected,
  request_completed,
  request_failed,
};

[[nodiscard]] inline std::string_view to_string(llm_dispatch_event_type type) noexcept {
  switch (type) {
    case llm_dispatch_event_type::client_selected:
      return "client_selected";
    case llm_dispatch_event_type::request_completed:
      return "request_completed";
    case llm_dispatch_event_type::request_failed:
      return "request_failed";
  }
  return "unknown";
}

struct llm_dispatch_event {
  llm_dispatch_event_type type { llm_dispatch_event_type::client_selected };
  std::string provider;
  std::string model;
  std::error_code error_code;
};

using llm_dispatch_observer = std::function<void(const llm_dispatch_event&)>;

struct dispatching_llm_client_options {
  std::string default_provider;
  bool allow_single_provider_fallback { true };
  llm_dispatch_observer observer;
};

class dispatching_llm_client final : public ::wuwe::llm_client,
                                     public llm_client_registry_dependency {
public:
  explicit dispatching_llm_client(
    std::shared_ptr<llm_client_registry> registry, dispatching_llm_client_options options = {})
      : registry_(std::move(registry)), options_(std::move(options)),
        identity_(std::make_shared<const detail::llm_dispatch_identity>()) {
    if (!registry_) {
      throw std::invalid_argument("dispatching_llm_client requires a registry");
    }
  }

  llm_response complete(const llm_request& request) override {
    return complete(request, {});
  }

  llm_response complete(const llm_request& request, std::stop_token stop_token) override {
    if (stop_token.stop_requested()) {
      auto response = cancelled_response();
      annotate(response, request.provider, request.model);
      publish({
        .type = llm_dispatch_event_type::request_failed,
        .provider = request.provider,
        .model = request.model,
        .error_code = response.error_code,
      });
      return response;
    }
    if (lineage_contains(request)) {
      return dispatch_cycle_failure(request);
    }
    const auto selected = select(request);
    if (!selected.client) {
      return selection_failure(request, selected);
    }
    auto dispatched_request = request;
    dispatched_request.provider = selected.provider;
    dispatched_request.runtime_context = extend_runtime_context(request);
    if (auto rejected =
          llm_request_rejection(dispatched_request, selected.client->capabilities())) {
      annotate(*rejected, selected.provider, request.model);
      publish({
        .type = llm_dispatch_event_type::request_failed,
        .provider = selected.provider,
        .model = request.model,
        .error_code = rejected->error_code,
      });
      return std::move(*rejected);
    }

    publish({
      .type = llm_dispatch_event_type::client_selected,
      .provider = selected.provider,
      .model = request.model,
    });
    llm_response response;
    try {
      response = selected.client->complete(dispatched_request, stop_token);
    }
    catch (...) {
      publish({
        .type = llm_dispatch_event_type::request_failed,
        .provider = selected.provider,
        .model = request.model,
        .error_code = agent::make_error_code(agent::llm_error_code::api_error),
      });
      throw;
    }
    annotate(response, selected.provider, request.model);
    publish({
      .type = response ? llm_dispatch_event_type::request_completed
                       : llm_dispatch_event_type::request_failed,
      .provider = selected.provider,
      .model = request.model,
      .error_code = response.error_code,
    });
    return response;
  }

  bool supports_streaming() const noexcept override {
    try {
      capability_scope scope(identity_.get());
      if (!scope) {
        return false;
      }
      const auto bindings = registry_->snapshot();
      for (const auto& binding : bindings) {
        if (binding.client->supports_streaming()) {
          return true;
        }
      }
    }
    catch (...) {
    }
    return false;
  }

  [[nodiscard]] llm_provider_capabilities capabilities() const noexcept override {
    try {
      capability_scope scope(identity_.get());
      if (!scope) {
        return { .declared = false };
      }
      const auto bindings = registry_->snapshot();
      if (bindings.empty()) {
        return { .declared = false };
      }

      llm_provider_capabilities aggregate;
      aggregate.declared = true;
      for (const auto& binding : bindings) {
        const auto current = binding.client->capabilities();
        aggregate.declared = aggregate.declared && current.declared;
        aggregate.streaming = aggregate.streaming || current.streaming;
        aggregate.tools = aggregate.tools || current.tools;
        aggregate.tool_choice = aggregate.tool_choice || current.tool_choice;
        aggregate.json_response_format =
          aggregate.json_response_format || current.json_response_format;
        aggregate.reasoning_summary = aggregate.reasoning_summary || current.reasoning_summary;
        aggregate.streaming_reasoning_summary =
          aggregate.streaming_reasoning_summary || current.streaming_reasoning_summary;
        if (static_cast<int>(current.reasoning_language_control) >
            static_cast<int>(aggregate.reasoning_language_control)) {
          aggregate.reasoning_language_control = current.reasoning_language_control;
        }
        aggregate.multimodal_input = aggregate.multimodal_input || current.multimodal_input;
        aggregate.local_runtime = aggregate.local_runtime || current.local_runtime;
        aggregate.stop_sequences = aggregate.stop_sequences || current.stop_sequences;
        aggregate.deterministic_seed = aggregate.deterministic_seed || current.deterministic_seed;
        aggregate.json_schema_output = aggregate.json_schema_output || current.json_schema_output;
        aggregate.explicit_cache_control =
          aggregate.explicit_cache_control || current.explicit_cache_control;
      }
      return aggregate;
    }
    catch (...) {
      return { .declared = false };
    }
  }

  llm_response complete_stream(const llm_request& request, const llm_stream_callbacks& callbacks,
    std::stop_token stop_token = {}) override {
    if (stop_token.stop_requested()) {
      auto response = cancelled_response();
      annotate(response, request.provider, request.model);
      publish({
        .type = llm_dispatch_event_type::request_failed,
        .provider = request.provider,
        .model = request.model,
        .error_code = response.error_code,
      });
      emit_llm_request_rejection(callbacks, response);
      return response;
    }
    if (lineage_contains(request)) {
      auto response = dispatch_cycle_failure(request);
      emit_llm_request_rejection(callbacks, response);
      return response;
    }
    const auto selected = select(request);
    if (!selected.client) {
      auto response = selection_failure(request, selected);
      emit_llm_request_rejection(callbacks, response);
      return response;
    }
    auto dispatched_request = request;
    dispatched_request.provider = selected.provider;
    dispatched_request.runtime_context = extend_runtime_context(request);
    if (auto rejected =
          llm_request_rejection(dispatched_request, selected.client->capabilities())) {
      annotate(*rejected, selected.provider, request.model);
      publish({
        .type = llm_dispatch_event_type::request_failed,
        .provider = selected.provider,
        .model = request.model,
        .error_code = rejected->error_code,
      });
      emit_llm_request_rejection(callbacks, *rejected);
      return std::move(*rejected);
    }

    auto callback_state =
      std::make_shared<stream_proxy_state>(callbacks, selected.provider, request.model);
    auto forwarded_callbacks = make_stream_proxy(callback_state);
    publish({
      .type = llm_dispatch_event_type::client_selected,
      .provider = selected.provider,
      .model = request.model,
    });
    llm_response response;
    try {
      response =
        selected.client->complete_stream(dispatched_request, forwarded_callbacks, stop_token);
    }
    catch (...) {
      publish({
        .type = llm_dispatch_event_type::request_failed,
        .provider = selected.provider,
        .model = request.model,
        .error_code = agent::make_error_code(agent::llm_error_code::api_error),
      });
      if (auto callback_failure = callback_state->failure()) {
        std::rethrow_exception(callback_failure);
      }
      throw;
    }
    annotate(response, selected.provider, request.model);
    publish({
      .type = response ? llm_dispatch_event_type::request_completed
                       : llm_dispatch_event_type::request_failed,
      .provider = selected.provider,
      .model = request.model,
      .error_code = response.error_code,
    });
    if (auto callback_failure = callback_state->failure()) {
      std::rethrow_exception(callback_failure);
    }
    return response;
  }

  [[nodiscard]] std::shared_ptr<llm_client_registry> registry() const noexcept {
    return registry_;
  }

private:
  struct capability_scope_tag {};
  struct observer_scope_tag {};

  template<typename Tag>
  class instance_scope {
  public:
    explicit instance_scope(const void* owner) : owner_(owner) {
      auto& active = active_dispatchers();
      if (std::find(active.begin(), active.end(), owner_) != active.end()) {
        return;
      }
      active.push_back(owner_);
      entered_ = true;
    }

    instance_scope(const instance_scope&) = delete;
    instance_scope& operator=(const instance_scope&) = delete;

    ~instance_scope() {
      if (!entered_) {
        return;
      }
      auto& active = active_dispatchers();
      if (!active.empty() && active.back() == owner_) {
        active.pop_back();
        return;
      }
      const auto found = std::find(active.begin(), active.end(), owner_);
      if (found != active.end()) {
        active.erase(found);
      }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
      return entered_;
    }

  private:
    static std::vector<const void*>& active_dispatchers() {
      static thread_local std::vector<const void*> active;
      return active;
    }

    const void* owner_ {};
    bool entered_ { false };
  };

  using capability_scope = instance_scope<capability_scope_tag>;
  using observer_scope = instance_scope<observer_scope_tag>;

  struct client_selection {
    std::string provider;
    std::shared_ptr<::wuwe::llm_client> client;
    agent::llm_error_code error { agent::llm_error_code::none };
    std::string message;
  };

  struct stream_proxy_state {
    stream_proxy_state(
      llm_stream_callbacks destination_value, std::string provider_value, std::string model_value)
        : destination(std::move(destination_value)), provider(std::move(provider_value)),
          model(std::move(model_value)) {
    }

    void forward_event(const llm_stream_event& event) {
      std::scoped_lock lock(mutex);
      if (!destination.on_event || callback_failure) {
        return;
      }
      try {
        auto forwarded = event;
        if (forwarded.response) {
          annotate(*forwarded.response, provider, model);
        }
        destination.on_event(forwarded);
      }
      catch (...) {
        callback_failure = std::current_exception();
      }
    }

    void forward_reasoning_delta(std::string_view delta) {
      std::scoped_lock lock(mutex);
      if (!destination.on_reasoning_delta || callback_failure) {
        return;
      }
      try {
        destination.on_reasoning_delta(delta);
      }
      catch (...) {
        callback_failure = std::current_exception();
      }
    }

    void forward_reasoning_done(std::string_view summary) {
      std::scoped_lock lock(mutex);
      if (!destination.on_reasoning_done || callback_failure) {
        return;
      }
      try {
        destination.on_reasoning_done(summary);
      }
      catch (...) {
        callback_failure = std::current_exception();
      }
    }

    [[nodiscard]] std::exception_ptr failure() const {
      std::scoped_lock lock(mutex);
      return callback_failure;
    }

    llm_stream_callbacks destination;
    std::string provider;
    std::string model;
    mutable std::mutex mutex;
    std::exception_ptr callback_failure;
  };

  [[nodiscard]] client_selection select(const llm_request& request) const {
    auto provider = request.provider;
    if (provider.empty()) {
      provider = options_.default_provider;
    }

    if (provider.empty() && options_.allow_single_provider_fallback) {
      const auto bindings = registry_->snapshot();
      if (bindings.size() == 1) {
        return {
          .provider = bindings.front().provider,
          .client = bindings.front().client,
        };
      }
      if (bindings.empty()) {
        return {
          .error = agent::llm_error_code::model_unavailable,
          .message = "No LLM clients are registered.",
        };
      }
    }

    if (provider.empty()) {
      return {
        .error = agent::llm_error_code::invalid_request,
        .message = "LLM request must select a provider when multiple clients are registered.",
      };
    }

    auto client = registry_->find(provider);
    if (!client) {
      return {
        .provider = std::move(provider),
        .error = agent::llm_error_code::model_unavailable,
        .message = "No LLM client is registered for the selected provider.",
      };
    }
    if (client.get() == this) {
      return {
        .provider = std::move(provider),
        .error = agent::llm_error_code::invalid_request,
        .message = "An LLM dispatcher cannot dispatch a request to itself.",
      };
    }
    return {
      .provider = std::move(provider),
      .client = std::move(client),
    };
  }

  llm_response selection_failure(
    const llm_request& request, const client_selection& selected) const {
    llm_response response {
      .content = selected.message,
      .error_code = agent::make_error_code(selected.error),
    };
    annotate(response, selected.provider, request.model);
    publish({
      .type = llm_dispatch_event_type::request_failed,
      .provider = selected.provider,
      .model = request.model,
      .error_code = response.error_code,
    });
    return response;
  }

  llm_response dispatch_cycle_failure(const llm_request& request) const {
    return selection_failure(request,
      {
        .provider = request.provider,
        .error = agent::llm_error_code::invalid_request,
        .message = "LLM dispatch cycle detected.",
      });
  }

  [[nodiscard]] bool lineage_contains(const llm_request& request) const noexcept {
    auto lineage = request.runtime_context ? request.runtime_context->dispatch_lineage
                                           : std::shared_ptr<const detail::llm_dispatch_lineage> {};
    while (lineage) {
      if (lineage->dispatcher == identity_) {
        return true;
      }
      lineage = lineage->parent;
    }
    return false;
  }

  [[nodiscard]] std::shared_ptr<const detail::llm_request_runtime_context> extend_runtime_context(
    const llm_request& request) const {
    auto context =
      request.runtime_context
        ? std::make_shared<detail::llm_request_runtime_context>(*request.runtime_context)
        : std::make_shared<detail::llm_request_runtime_context>();
    context->dispatch_lineage =
      std::make_shared<const detail::llm_dispatch_lineage>(identity_, context->dispatch_lineage);
    return context;
  }

  [[nodiscard]] bool references_registry(
    const llm_client_registry* registry, std::vector<const void*>& traversal) const override {
    if (registry_.get() == registry) {
      return true;
    }
    if (std::find(traversal.begin(), traversal.end(), identity_.get()) != traversal.end()) {
      return false;
    }
    traversal.push_back(identity_.get());
    for (const auto& binding : registry_->snapshot()) {
      const auto* dependency =
        dynamic_cast<const llm_client_registry_dependency*>(binding.client.get());
      if (dependency && dependency->references_registry(registry, traversal)) {
        traversal.pop_back();
        return true;
      }
    }
    traversal.pop_back();
    return false;
  }

  static llm_stream_callbacks make_stream_proxy(const std::shared_ptr<stream_proxy_state>& state) {
    llm_stream_callbacks proxy;
    proxy.on_event = [state](const llm_stream_event& event) { state->forward_event(event); };
    proxy.on_reasoning_delta = [state](
                                 std::string_view delta) { state->forward_reasoning_delta(delta); };
    proxy.on_reasoning_done = [state](std::string_view summary) {
      state->forward_reasoning_done(summary);
    };
    return proxy;
  }

  static llm_response cancelled_response() {
    return {
      .content = "LLM request cancelled.",
      .error_code = agent::make_error_code(agent::llm_error_code::cancelled),
    };
  }

  static void annotate(
    llm_response& response, const std::string& provider, const std::string& model) {
    if (!provider.empty()) {
      response.metadata.insert_or_assign("wuwe.dispatch.provider", provider);
    }
    if (!model.empty()) {
      response.metadata.insert_or_assign("wuwe.dispatch.model", model);
    }
  }

  void publish(llm_dispatch_event event) const noexcept {
    if (!options_.observer) {
      return;
    }
    observer_scope scope(identity_.get());
    if (!scope) {
      return;
    }
    try {
      options_.observer(event);
    }
    catch (...) {
    }
  }

  std::shared_ptr<llm_client_registry> registry_;
  dispatching_llm_client_options options_;
  std::shared_ptr<const detail::llm_dispatch_identity> identity_;
};

} // namespace wuwe::agent::llm

#endif // WUWE_AGENT_LLM_DISPATCHING_LLM_CLIENT_HPP
