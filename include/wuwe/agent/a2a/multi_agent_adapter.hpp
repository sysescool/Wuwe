#ifndef WUWE_AGENT_A2A_MULTI_AGENT_ADAPTER_HPP
#define WUWE_AGENT_A2A_MULTI_AGENT_ADAPTER_HPP

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/a2a/a2a_client.hpp>
#include <wuwe/agent/multi_agent/multi_agent.hpp>

namespace wuwe::agent::a2a {

inline multi_agent::agent_descriptor agent_descriptor_from_card(
  const agent_card& card,
  std::string id = {}) {
  multi_agent::agent_descriptor descriptor {
    .id = id.empty() ? card.name : std::move(id),
    .name = card.name,
    .role = "remote",
    .description = card.description,
    .max_concurrency = 1,
    .remote = true,
    .metadata = {
      { "a2a_url", card.url },
      { "a2a_version", card.version },
      { "a2a_protocol_version", card.protocol_version },
    },
  };
  descriptor.skills.reserve(card.skills.size());
  for (const auto& skill : card.skills) {
    descriptor.skills.push_back({
      .id = skill.id,
      .name = skill.name,
      .description = skill.description,
      .tags = skill.tags,
      .input_modes = skill.input_modes,
      .output_modes = skill.output_modes,
    });
  }
  return descriptor;
}

class remote_agent_executor final : public multi_agent::agent_executor {
public:
  explicit remote_agent_executor(std::shared_ptr<client> client)
      : client_(std::move(client)) {
    if (!client_) {
      throw std::invalid_argument("remote A2A agent executor requires a client");
    }
  }

  multi_agent::agent_task_result execute(
    const multi_agent::agent_task_request& request,
    const multi_agent::agent_execution_context& context) override {
    message outbound {
      .message_id = request.id + ":message",
      .role = message_role::user,
      .parts = { part::text_part(request.input) },
      .task_id = request.id,
      .context_id = request.session_id,
      .metadata = request.metadata,
    };
    for (const auto& source : request.messages) {
      if (!source.content.empty()) {
        outbound.parts.push_back(part::text_part(source.content));
      }
    }
    const auto remote = client_->send({
      .value = std::move(outbound),
      .configuration = { .blocking = true },
      .metadata = request.metadata,
    }, context.stop_token);
    if (!remote) {
      return {
        .status = context.stop_token.stop_requested()
                    ? multi_agent::agent_task_status::cancelled
                    : multi_agent::agent_task_status::failed,
        .error_code = context.stop_token.stop_requested()
                        ? multi_agent::agent_task_error_code::cancelled
                        : multi_agent::agent_task_error_code::execution_failed,
        .error = remote.failure ? remote.failure->message : "remote A2A agent failed",
      };
    }
    return from_a2a_task(*remote.value);
  }

  [[nodiscard]] multi_agent::agent_executor_capabilities capabilities() const noexcept override {
    return {
      .cooperative_cancellation = client_->capabilities().cooperative_cancellation,
      .concurrent_execution = client_->capabilities().concurrent_invocation,
    };
  }

private:
  static multi_agent::agent_task_status map_status(task_state state) {
    switch (state) {
      case task_state::submitted: return multi_agent::agent_task_status::submitted;
      case task_state::working: return multi_agent::agent_task_status::working;
      case task_state::input_required: return multi_agent::agent_task_status::input_required;
      case task_state::completed: return multi_agent::agent_task_status::completed;
      case task_state::canceled: return multi_agent::agent_task_status::cancelled;
      case task_state::rejected: return multi_agent::agent_task_status::blocked;
      case task_state::failed:
      case task_state::auth_required:
      case task_state::unknown:
        return multi_agent::agent_task_status::failed;
    }
    return multi_agent::agent_task_status::failed;
  }

  static std::string text_content(const std::vector<part>& parts) {
    std::string output;
    for (const auto& item : parts) {
      if (item.kind == part_kind::text) {
        if (!output.empty()) output.push_back('\n');
        output += item.text;
      }
      else if (item.kind == part_kind::data) {
        if (!output.empty()) output.push_back('\n');
        output += item.data.dump();
      }
    }
    return output;
  }

  static multi_agent::agent_task_result from_a2a_task(const task& remote) {
    multi_agent::agent_task_result output {
      .task_id = remote.id,
      .session_id = remote.context_id,
      .status = map_status(remote.status.state),
      .metadata = metadata_strings(remote.metadata),
    };
    if (remote.status.status_message) {
      output.error = text_content(remote.status.status_message->parts);
    }
    for (const auto& remote_artifact : remote.artifacts) {
      multi_agent::agent_artifact artifact {
        .id = remote_artifact.artifact_id,
        .name = remote_artifact.name,
        .description = remote_artifact.description,
        .metadata = metadata_strings(remote_artifact.metadata),
      };
      artifact.content = text_content(remote_artifact.parts);
      for (const auto& item : remote_artifact.parts) {
        if (item.kind == part_kind::data) {
          artifact.data = item.data;
          artifact.mime_type = "application/json";
          break;
        }
      }
      output.artifacts.push_back(std::move(artifact));
    }
    if (!output.artifacts.empty()) {
      output.output = output.artifacts.front().content;
      if (output.output.empty() && !output.artifacts.front().data.is_null()) {
        output.output = output.artifacts.front().data.dump();
      }
    }
    if (output.status == multi_agent::agent_task_status::completed &&
        output.output.empty() && remote.status.status_message) {
      output.output = text_content(remote.status.status_message->parts);
    }
    if (output.status != multi_agent::agent_task_status::completed) {
      output.error_code = output.status == multi_agent::agent_task_status::cancelled
                            ? multi_agent::agent_task_error_code::cancelled
                            : output.status == multi_agent::agent_task_status::blocked
                                ? multi_agent::agent_task_error_code::agent_unavailable
                                : multi_agent::agent_task_error_code::execution_failed;
    }
    return output;
  }

  static std::map<std::string, std::string> metadata_strings(
    const nlohmann::json& source) {
    if (!source.is_object()) {
      throw std::invalid_argument("A2A metadata must be an object");
    }
    std::map<std::string, std::string> output;
    for (auto item = source.begin(); item != source.end(); ++item) {
      output[item.key()] = item.value().is_string()
                             ? item.value().get<std::string>()
                             : item.value().dump();
    }
    return output;
  }

  std::shared_ptr<client> client_;
};

struct team_task_handler_options {
  std::shared_ptr<multi_agent::team_runtime> runtime;
  std::string preferred_agent;
  std::vector<std::string> required_skills;
  std::size_t max_background_tasks { 32 };
};

class team_task_handler final : public task_handler {
public:
  explicit team_task_handler(team_task_handler_options options)
      : options_(std::move(options)) {
    if (!options_.runtime) {
      throw std::invalid_argument("A2A team task handler requires a team runtime");
    }
    if (options_.max_background_tasks == 0) {
      throw std::invalid_argument(
        "A2A team task handler max_background_tasks must be greater than zero");
    }
    background_.reserve(options_.max_background_tasks);
  }

  ~team_task_handler() override {
    std::vector<std::future<void>> background;
    try {
      std::scoped_lock lock(mutex_);
      for (auto& [_, record] : tasks_) {
        if (record.stop_source) {
          record.stop_source->request_stop();
        }
      }
      background.swap(background_);
    }
    catch (...) {
    }
    for (auto& task : background) {
      try {
        task.get();
      }
      catch (...) {
      }
    }
  }

  result<task> send(
    const send_message_params& params,
    std::stop_token transport_stop_token) override {
    const auto task_id = params.value.task_id.empty()
                           ? params.value.message_id
                           : params.value.task_id;
    if (task_id.empty()) {
      return failure(error_code::invalid_params, "A2A message requires a task or message id");
    }
    const auto metadata = local_metadata(params.value.metadata, params.metadata);
    const auto input = message_content(params.value);
    auto stop_source = std::make_shared<std::stop_source>();
    std::optional<std::stop_callback<std::function<void()>>> transport_stop;
    if (transport_stop_token.stop_possible()) {
      transport_stop.emplace(transport_stop_token, std::function<void()>([stop_source] {
        stop_source->request_stop();
      }));
    }
    task working;
    multi_agent::agent_task_request local_request;
    std::optional<task_record> previous;
    {
      std::scoped_lock lock(mutex_);
      reap_background_locked();
      const auto found = tasks_.find(task_id);
      if (found != tasks_.end()) {
        if (found->second.value.status.state != task_state::input_required) {
          return failure(error_code::invalid_params, "duplicate A2A task id: " + task_id);
        }
        if (!params.value.context_id.empty() &&
            params.value.context_id != found->second.value.context_id) {
          return failure(
            error_code::invalid_params,
            "A2A continuation context does not match the existing task");
        }
        previous = found->second;
        working = found->second.value;
        working.status = {
          .state = params.configuration.blocking
                     ? task_state::working
                     : task_state::submitted,
        };
        working.history.push_back(params.value);
        merge_metadata(working.metadata, params.metadata);
      }
      else {
        working = {
          .id = task_id,
          .context_id = params.value.context_id.empty()
                          ? task_id
                          : params.value.context_id,
          .status = {
            .state = params.configuration.blocking
                       ? task_state::working
                       : task_state::submitted,
          },
          .history = { params.value },
          .metadata = params.metadata,
        };
      }
      local_request = {
        .id = task_id,
        .session_id = working.context_id,
        .input = input,
        .required_skills = options_.required_skills,
        .preferred_agent = options_.preferred_agent,
        .metadata = metadata,
      };
      if (!params.configuration.blocking) {
        if (background_.size() >= options_.max_background_tasks) {
          return failure(
            error_code::internal_error,
            "A2A background task capacity exhausted",
            nlohmann::json {
              { "reason", "background_capacity_exhausted" },
              { "retryable", true },
              { "maximum", options_.max_background_tasks },
            });
        }
      }
      tasks_[task_id] = { .value = working, .stop_source = stop_source };
      if (!params.configuration.blocking) {
        try {
          background_.push_back(std::async(
            std::launch::async,
            [this,
             working,
             local_request = std::move(local_request),
             stop_source]() mutable {
              const auto task_id = working.id;
              try {
                (void)execute_and_store(
                  std::move(working), std::move(local_request), std::move(stop_source));
              }
              catch (const std::exception& ex) {
                fail_background_task(task_id, ex.what());
              }
              catch (...) {
                fail_background_task(
                  task_id, "local team failed with an unknown background exception");
              }
            }));
        }
        catch (const std::exception& ex) {
          if (previous) tasks_[task_id] = std::move(*previous);
          else tasks_.erase(task_id);
          return failure(
            error_code::internal_error,
            std::string("failed to start A2A background task: ") + ex.what(),
            nlohmann::json {
              { "reason", "background_task_start_failed" },
              { "retryable", true },
            });
        }
        catch (...) {
          if (previous) tasks_[task_id] = std::move(*previous);
          else tasks_.erase(task_id);
          return failure(
            error_code::internal_error,
            "failed to start A2A background task",
            nlohmann::json {
              { "reason", "background_task_start_failed" },
              { "retryable", true },
            });
        }
        return { .value = working };
      }
    }
    return { .value = execute_and_store(
      std::move(working), std::move(local_request), std::move(stop_source)) };
  }

  result<task> get(const task_query_params& params) override {
    std::scoped_lock lock(mutex_);
    reap_background_locked();
    const auto found = tasks_.find(params.id);
    if (found == tasks_.end()) {
      return failure(error_code::task_not_found, "A2A task not found: " + params.id);
    }
    auto output = found->second.value;
    if (params.history_length != 0 && output.history.size() > params.history_length) {
      output.history.erase(
        output.history.begin(),
        output.history.end() - static_cast<std::ptrdiff_t>(params.history_length));
    }
    return { .value = std::move(output) };
  }

  result<task> cancel(const task_id_params& params) override {
    std::scoped_lock lock(mutex_);
    reap_background_locked();
    const auto found = tasks_.find(params.id);
    if (found == tasks_.end()) {
      return failure(error_code::task_not_found, "A2A task not found: " + params.id);
    }
    if (found->second.value.status.state == task_state::completed ||
        found->second.value.status.state == task_state::failed ||
        found->second.value.status.state == task_state::canceled ||
        found->second.value.status.state == task_state::rejected) {
      return failure(
        error_code::task_not_cancelable,
        "A2A task is not cancelable: " + params.id);
    }
    if (found->second.value.status.state == task_state::input_required) {
      mark_cancelled(found->second.value);
      return { .value = found->second.value };
    }
    if (!found->second.stop_source) {
      return failure(
        error_code::task_not_cancelable,
        "A2A task is not cancelable: " + params.id);
    }
    found->second.stop_source->request_stop();
    mark_cancelled(found->second.value);
    return { .value = found->second.value };
  }

private:
  struct task_record {
    task value;
    std::shared_ptr<std::stop_source> stop_source;
  };

  static result<task> failure(
    error_code code,
    std::string message,
    std::optional<nlohmann::json> data = std::nullopt) {
    return { .failure = error {
      .code = code,
      .message = std::move(message),
      .data = std::move(data),
    } };
  }

  task execute_and_store(
    task base,
    multi_agent::agent_task_request request,
    std::shared_ptr<std::stop_source> stop_source) {
    task completed;
    {
      std::scoped_lock lock(mutex_);
      const auto found = tasks_.find(base.id);
      if (found != tasks_.end() &&
          found->second.value.status.state != task_state::canceled) {
        found->second.value.status.state = task_state::working;
      }
    }
    try {
      auto local = options_.runtime->run(
        std::move(request), stop_source->get_token());
      if (stop_source->stop_requested()) {
        local.status = multi_agent::agent_task_status::cancelled;
        local.error_code = multi_agent::agent_task_error_code::cancelled;
        if (local.error.empty()) {
          local.error = "A2A task cancelled";
        }
        local.output.clear();
        local.artifacts.clear();
      }
      completed = from_local_result(std::move(base), local);
    }
    catch (const std::exception& ex) {
      base.status.state = task_state::failed;
      base.status.status_message = message {
        .message_id = base.id + ":status",
        .role = message_role::agent,
        .parts = { part::text_part(ex.what()) },
        .task_id = base.id,
        .context_id = base.context_id,
      };
      completed = std::move(base);
    }
    catch (...) {
      base.status.state = task_state::failed;
      base.status.status_message = message {
        .message_id = base.id + ":status",
        .role = message_role::agent,
        .parts = { part::text_part("local team failed with an unknown exception") },
        .task_id = base.id,
        .context_id = base.context_id,
      };
      completed = std::move(base);
    }
    std::scoped_lock lock(mutex_);
    const auto found = tasks_.find(completed.id);
    if (found != tasks_.end()) {
      if (found->second.value.status.state == task_state::canceled ||
          stop_source->stop_requested()) {
        mark_cancelled(completed);
      }
      found->second.value = completed;
      found->second.stop_source.reset();
    }
    return completed;
  }

  void reap_background_locked() {
    for (auto it = background_.begin(); it != background_.end();) {
      if (it->wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        try {
          it->get();
        }
        catch (...) {
          // Background workers translate failures into task state before returning.
          // Keep reaping defensive so a future implementation cannot poison all
          // subsequent task operations with an escaped worker exception.
        }
        it = background_.erase(it);
      }
      else {
        ++it;
      }
    }
  }

  void fail_background_task(const std::string& task_id, std::string failure) noexcept {
    try {
      std::scoped_lock lock(mutex_);
      const auto found = tasks_.find(task_id);
      if (found == tasks_.end()) return;
      if (found->second.value.status.state != task_state::canceled) {
        found->second.value.status.state = task_state::failed;
        found->second.value.status.status_message = message {
          .message_id = task_id + ":status",
          .role = message_role::agent,
          .parts = { part::text_part(std::move(failure)) },
          .task_id = task_id,
          .context_id = found->second.value.context_id,
        };
      }
      found->second.stop_source.reset();
    }
    catch (...) {
    }
  }

  static std::string message_content(const message& value) {
    std::string output;
    for (const auto& item : value.parts) {
      if (!output.empty()) output.push_back('\n');
      if (item.kind == part_kind::text) output += item.text;
      else if (item.kind == part_kind::data) output += item.data.dump();
      else if (!item.file.uri.empty()) output += item.file.uri;
      else output += item.file.name;
    }
    return output;
  }

  static std::map<std::string, std::string> local_metadata(
    const nlohmann::json& message_metadata,
    const nlohmann::json& request_metadata) {
    if (!message_metadata.is_object() || !request_metadata.is_object()) {
      throw std::invalid_argument("A2A metadata must be an object");
    }
    std::map<std::string, std::string> output;
    const auto append = [&](const nlohmann::json& source) {
      for (auto item = source.begin(); item != source.end(); ++item) {
        output[item.key()] = item.value().is_string()
                               ? item.value().get<std::string>()
                               : item.value().dump();
      }
    };
    append(message_metadata);
    append(request_metadata);
    return output;
  }

  static void merge_metadata(
    nlohmann::json& target,
    const nlohmann::json& source) {
    if (!target.is_object() || !source.is_object()) {
      throw std::invalid_argument("A2A metadata must be an object");
    }
    for (auto item = source.begin(); item != source.end(); ++item) {
      target[item.key()] = item.value();
    }
  }

  static void mark_cancelled(task& value) {
    value.status.state = task_state::canceled;
    value.status.status_message = message {
      .message_id = value.id + ":status",
      .role = message_role::agent,
      .parts = { part::text_part("A2A task cancelled") },
      .task_id = value.id,
      .context_id = value.context_id,
    };
  }

  static task_state map_status(multi_agent::agent_task_status status) {
    switch (status) {
      case multi_agent::agent_task_status::submitted: return task_state::submitted;
      case multi_agent::agent_task_status::working: return task_state::working;
      case multi_agent::agent_task_status::input_required: return task_state::input_required;
      case multi_agent::agent_task_status::completed: return task_state::completed;
      case multi_agent::agent_task_status::cancelled: return task_state::canceled;
      case multi_agent::agent_task_status::timed_out: return task_state::failed;
      case multi_agent::agent_task_status::blocked: return task_state::rejected;
      case multi_agent::agent_task_status::failed: return task_state::failed;
    }
    return task_state::failed;
  }

  static task from_local_result(task base, const multi_agent::agent_task_result& local) {
    base.status.state = map_status(local.status);
    base.status.status_message.reset();
    if (!local.error.empty()) {
      base.status.status_message = message {
        .message_id = local.task_id + ":status",
        .role = message_role::agent,
        .parts = { part::text_part(local.error) },
        .task_id = local.task_id,
        .context_id = local.session_id,
      };
    }
    if (!local.output.empty()) {
      upsert_artifact(base, {
        .artifact_id = local.task_id + ":output",
        .name = "result",
        .parts = { part::text_part(local.output) },
      });
    }
    for (const auto& local_artifact : local.artifacts) {
      artifact converted {
        .artifact_id = local_artifact.id,
        .name = local_artifact.name,
        .description = local_artifact.description,
        .metadata = local_artifact.metadata,
      };
      if (!local_artifact.content.empty()) {
        converted.parts.push_back(part::text_part(local_artifact.content));
      }
      if (!local_artifact.data.is_null()) {
        converted.parts.push_back(part::data_part(local_artifact.data));
      }
      upsert_artifact(base, std::move(converted));
    }
    base.metadata["agentId"] = local.agent_id;
    return base;
  }

  static void upsert_artifact(task& target, artifact value) {
    const auto found = std::find_if(
      target.artifacts.begin(), target.artifacts.end(), [&](const auto& existing) {
        return existing.artifact_id == value.artifact_id;
      });
    if (found == target.artifacts.end()) {
      target.artifacts.push_back(std::move(value));
    }
    else {
      *found = std::move(value);
    }
  }

  team_task_handler_options options_;
  std::mutex mutex_;
  std::map<std::string, task_record> tasks_;
  std::vector<std::future<void>> background_;
};

} // namespace wuwe::agent::a2a

#endif // WUWE_AGENT_A2A_MULTI_AGENT_ADAPTER_HPP
