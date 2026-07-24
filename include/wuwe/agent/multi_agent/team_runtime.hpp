#ifndef WUWE_AGENT_MULTI_AGENT_TEAM_RUNTIME_HPP
#define WUWE_AGENT_MULTI_AGENT_TEAM_RUNTIME_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <wuwe/agent/multi_agent/detail/team_runtime_state.hpp>
#include <wuwe/agent/multi_agent/team_runtime_types.hpp>

namespace wuwe::agent::multi_agent {

class team_runtime {
public:
  explicit team_runtime(team_runtime_options options = {})
      : state_(std::make_shared<detail::team_runtime_state>(std::move(options))) {
  }

  [[nodiscard]] std::shared_ptr<agent_registry> registry() const noexcept {
    return state_->options.registry;
  }

  [[nodiscard]] std::shared_ptr<team_session> create_session(
    std::string id = {},
    std::map<std::string, std::string> metadata = {}) const {
    if (id.empty()) {
      id = detail::next_team_id("session");
    }
    auto session = std::make_shared<team_session>(id, std::move(metadata));
    std::scoped_lock lock(state_->sessions_mutex);
    if (state_->sessions.contains(id)) {
      throw std::invalid_argument("duplicate team session id: " + id);
    }
    state_->sessions.emplace(id, session);
    return session;
  }

  [[nodiscard]] std::shared_ptr<team_session> find_session(const std::string& id) const {
    std::scoped_lock lock(state_->sessions_mutex);
    const auto found = state_->sessions.find(id);
    return found == state_->sessions.end() ? nullptr : found->second;
  }

  [[nodiscard]] agent_task_result run(
    agent_task_request request,
    std::stop_token stop_token = {}) const {
    const auto started = std::chrono::steady_clock::now();
    const auto timeout = request.timeout.count() > 0
                           ? request.timeout
                           : state_->options.default_task_timeout;
    const auto deadline = timeout.count() > 0
                            ? std::optional(started + timeout)
                            : std::optional<std::chrono::steady_clock::time_point> {};
    if (request.id.empty()) {
      request.id = detail::next_team_id("task");
    }
    if (request.session_id.empty()) {
      request.session_id = detail::next_team_id("session");
    }
    auto session = get_or_create_session(request.session_id);
    if (!session->try_start_task(request.id)) {
      std::size_t telemetry_errors = emit({
        .type = team_event_type::task_blocked,
        .task_id = request.id,
        .session_id = request.session_id,
        .message = "duplicate or active agent task id",
        .metadata = request.metadata,
      });
      agent_task_result duplicate {
        .task_id = request.id,
        .session_id = request.session_id,
        .status = agent_task_status::blocked,
        .error_code = agent_task_error_code::invalid_request,
        .error = "duplicate or active agent task id: " + request.id,
        .elapsed = elapsed_since(started),
      };
      if (telemetry_errors != 0) {
        duplicate.metadata["telemetry_error_count"] =
          std::to_string(telemetry_errors);
      }
      return duplicate;
    }
    detail::active_team_task_guard task_guard(session, request.id);
    std::size_t telemetry_errors = emit({
      .type = team_event_type::task_submitted,
      .task_id = request.id,
      .session_id = request.session_id,
      .metadata = request.metadata,
    });

    if (request.input.empty() && request.messages.empty()) {
      return finish_error(
        request,
        session,
        started,
        agent_task_status::blocked,
        agent_task_error_code::invalid_request,
        "agent task requires input or messages",
        telemetry_errors);
    }
    if (stop_token.stop_requested()) {
      return finish_error(
        request,
        session,
        started,
        agent_task_status::cancelled,
        agent_task_error_code::cancelled,
        "agent task cancelled before dispatch",
        telemetry_errors);
    }

    auto runtime_slot = acquire_runtime_slot(stop_token, deadline);
    if (!runtime_slot) {
      const auto timed_out = deadline && std::chrono::steady_clock::now() >= *deadline;
      return finish_error(
        request,
        session,
        started,
        timed_out ? agent_task_status::timed_out : agent_task_status::cancelled,
        timed_out ? agent_task_error_code::timed_out : agent_task_error_code::cancelled,
        timed_out
          ? "agent task timed out while waiting for runtime capacity"
          : "agent task cancelled while waiting for runtime capacity",
        telemetry_errors);
    }

    auto acquired = state_->options.registry->acquire(
      request.preferred_agent,
      request.required_skills);
    if (!acquired) {
      return finish_error(
        request,
        session,
        started,
        agent_task_status::blocked,
        acquired.error,
        acquired.message,
        telemetry_errors);
    }
    auto lease = std::move(*acquired.lease);
    const auto agent_id = lease.descriptor().id;
    telemetry_errors += emit({
      .type = team_event_type::agent_selected,
      .task_id = request.id,
      .session_id = request.session_id,
      .agent_id = agent_id,
      .metadata = request.metadata,
    });
    session->update_task(request.id, agent_task_status::working);
    telemetry_errors += emit({
      .type = team_event_type::task_started,
      .task_id = request.id,
      .session_id = request.session_id,
      .agent_id = agent_id,
      .metadata = request.metadata,
    });

    if (!request.input.empty()) {
      session->publish({
        .id = request.id + ":input",
        .role = agent_message_role::user,
        .content = request.input,
      });
    }
    for (const auto& message : request.messages) {
      session->publish(message);
    }

    auto execution = execute_agent(
      std::move(lease),
      std::move(*runtime_slot),
      request,
      session,
      stop_token,
      deadline);
    agent_task_result result = std::move(execution.result);
    result.detached = execution.detached;

    result.task_id = request.id;
    result.session_id = request.session_id;
    result.agent_id = agent_id;
    if (stop_token.stop_requested() && result.status != agent_task_status::completed &&
        result.status != agent_task_status::timed_out) {
      result.status = agent_task_status::cancelled;
      result.error_code = agent_task_error_code::cancelled;
      if (result.error.empty()) {
        result.error = "agent task cancelled";
      }
    }
    if (result.status == agent_task_status::submitted ||
        result.status == agent_task_status::working) {
      const auto returned_status = to_string(result.status);
      result.status = agent_task_status::failed;
      result.error_code = agent_task_error_code::execution_failed;
      result.error = "synchronous agent executor returned non-terminal status: " +
                     returned_status;
      result.output.clear();
      result.messages.clear();
      result.artifacts.clear();
    }
    const auto invalid_artifact = std::find_if(
      result.artifacts.begin(), result.artifacts.end(), [](const auto& artifact) {
        return artifact.id.empty();
      });
    if (invalid_artifact != result.artifacts.end()) {
      result.status = agent_task_status::failed;
      result.error_code = agent_task_error_code::execution_failed;
      result.error = "agent executor returned an artifact without an id";
      result.output.clear();
      result.messages.clear();
      result.artifacts.clear();
    }
    if (result.status == agent_task_status::completed) {
      result.error_code = agent_task_error_code::none;
      result.error.clear();
    }
    else if (result.error_code == agent_task_error_code::none &&
             result.status != agent_task_status::submitted &&
             result.status != agent_task_status::working &&
             result.status != agent_task_status::input_required) {
      result.error_code = agent_task_error_code::execution_failed;
    }
    result.elapsed = elapsed_since(started);
    session->update_task(request.id, result.status);
    if (!result.output.empty()) {
      session->publish({
        .id = request.id + ":output",
        .role = agent_message_role::agent,
        .content = result.output,
        .author_agent = agent_id,
      });
    }
    for (const auto& message : result.messages) {
      session->publish(message);
    }
    for (const auto& artifact : result.artifacts) {
      session->publish(artifact);
    }

    const auto event_type = result.status == agent_task_status::completed
                              ? team_event_type::task_completed
                              : result.status == agent_task_status::timed_out
                                  ? team_event_type::task_timed_out
                              : result.status == agent_task_status::cancelled
                                  ? team_event_type::task_cancelled
                                  : result.status == agent_task_status::blocked ||
                                      result.status == agent_task_status::input_required
                                      ? team_event_type::task_blocked
                                      : team_event_type::task_failed;
    telemetry_errors += emit({
      .type = event_type,
      .task_id = request.id,
      .session_id = request.session_id,
      .agent_id = agent_id,
      .message = result.error,
      .elapsed = result.elapsed,
      .metadata = request.metadata,
    });
    result.metadata["status"] = to_string(result.status);
    if (telemetry_errors != 0) {
      result.metadata["telemetry_error_count"] = std::to_string(telemetry_errors);
    }
    return result;
  }

  [[nodiscard]] std::future<agent_task_result> run_async(
    agent_task_request request,
    std::stop_token stop_token = {}) const {
    return std::async(
      std::launch::async,
      [runtime = *this, request = std::move(request), stop_token]() mutable {
        return runtime.run(std::move(request), stop_token);
      });
  }

  [[nodiscard]] std::vector<agent_task_result> run_parallel(
    std::vector<agent_task_request> requests,
    std::stop_token stop_token = {}) const {
    std::vector<agent_task_result> output(requests.size());
    std::vector<std::exception_ptr> failures(requests.size());
    std::atomic<std::size_t> next { 0 };
    const auto worker_count = (std::min)(
      requests.size(), state_->options.max_parallel_tasks);
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    const auto coordinate = [&, runtime = *this] {
      for (;;) {
        const auto index = next.fetch_add(1, std::memory_order_relaxed);
        if (index >= requests.size()) return;
        try {
          output[index] = runtime.run(std::move(requests[index]), stop_token);
        }
        catch (...) {
          failures[index] = std::current_exception();
        }
      }
    };
    bool worker_start_failed = false;
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
      try {
        workers.emplace_back(coordinate);
      }
      catch (...) {
        worker_start_failed = true;
        break;
      }
    }
    if (worker_start_failed || (worker_count != 0 && workers.empty())) {
      coordinate();
    }
    workers.clear();
    for (const auto& failure : failures) {
      if (failure) std::rethrow_exception(failure);
    }
    return output;
  }

  [[nodiscard]] consensus_result reach_consensus(
    consensus_request request,
    std::stop_token stop_token = {}) const {
    if (request.task.session_id.empty()) {
      request.task.session_id = detail::next_team_id("session");
    }
    if (request.task.id.empty()) {
      request.task.id = detail::next_team_id("consensus");
    }
    auto session = get_or_create_session(request.task.session_id);
    if (!session->try_start_task(request.task.id)) {
      emit({
        .type = team_event_type::consensus_failed,
        .task_id = request.task.id,
        .session_id = request.task.session_id,
        .message = "duplicate or active consensus task id",
        .metadata = request.task.metadata,
      });
      return {
        .error_code = agent_task_error_code::invalid_request,
        .error = "duplicate or active consensus task id: " + request.task.id,
      };
    }
    detail::active_team_task_guard task_guard(session, request.task.id);
    if (stop_token.stop_requested()) {
      return consensus_failure(
        request.task,
        agent_task_error_code::cancelled,
        "consensus cancelled before dispatch",
        agent_task_status::cancelled);
    }
    auto participants = request.participant_agents;
    if (participants.empty()) {
      for (const auto& registered : state_->options.registry->list()) {
        if (registered.availability == agent_availability::available &&
            detail::has_skills(registered.descriptor, request.task.required_skills)) {
          participants.push_back(registered.descriptor.id);
        }
      }
    }
    std::sort(participants.begin(), participants.end());
    if (std::adjacent_find(participants.begin(), participants.end()) != participants.end()) {
      return consensus_failure(
        request.task,
        agent_task_error_code::invalid_request,
        "consensus participant agents must be unique",
        agent_task_status::blocked);
    }
    if (participants.empty()) {
      return consensus_failure(
        request.task,
        agent_task_error_code::agent_not_found,
        "consensus requires at least one participant",
        agent_task_status::blocked);
    }
    const auto minimum = request.minimum_successful_agents == 0
                           ? std::size_t { 1 }
                           : request.minimum_successful_agents;
    if (minimum > participants.size()) {
      return consensus_failure(
        request.task,
        agent_task_error_code::invalid_request,
        "minimum successful agents exceeds participant count",
        agent_task_status::blocked);
    }
    auto consensus_metadata = request.task.metadata;
    consensus_metadata["participant_count"] = std::to_string(participants.size());
    std::size_t telemetry_errors = emit({
      .type = team_event_type::consensus_started,
      .task_id = request.task.id,
      .session_id = request.task.session_id,
      .metadata = std::move(consensus_metadata),
    });
    session->update_task(request.task.id, agent_task_status::working);

    std::vector<agent_task_request> tasks;
    tasks.reserve(participants.size());
    const auto round_id = detail::next_team_id("consensus-round");
    for (const auto& participant : participants) {
      auto task = request.task;
      task.id = request.task.id + ":" + participant + ":" + round_id;
      task.preferred_agent = participant;
      task.metadata["consensus_task_id"] = request.task.id;
      task.metadata["consensus_round_id"] = round_id;
      tasks.push_back(std::move(task));
    }
    auto results = run_parallel(std::move(tasks), stop_token);
    const auto successful = static_cast<std::size_t>(std::count_if(
      results.begin(), results.end(), [](const auto& result) {
        return static_cast<bool>(result);
      }));
    if (stop_token.stop_requested() && successful < minimum) {
      auto failure = consensus_failure(
        request.task,
        agent_task_error_code::cancelled,
        "consensus cancelled during participant execution",
        agent_task_status::cancelled);
      failure.results = std::move(results);
      return failure;
    }
    if (successful < minimum) {
      auto failure = consensus_failure(
        request.task,
        agent_task_error_code::consensus_not_reached,
        "not enough agents completed successfully");
      failure.results = std::move(results);
      return failure;
    }

    const auto required_agreement = request.minimum_agreement == 0
                                      ? successful / 2 + 1
                                      : request.minimum_agreement;
    if (required_agreement > successful) {
      auto failure = consensus_failure(
        request.task,
        agent_task_error_code::consensus_not_reached,
        "minimum agreement exceeds successful agent count");
      failure.results = std::move(results);
      return failure;
    }
    agent_task_result final_result;
    try {
      final_result = request.resolver
        ? request.resolver(results, session->snapshot())
        : exact_output_consensus(results, required_agreement);
    }
    catch (const std::exception& ex) {
      auto failure = consensus_failure(
        request.task,
        agent_task_error_code::execution_failed,
        std::string("consensus resolver failed: ") + ex.what());
      failure.results = std::move(results);
      return failure;
    }
    catch (...) {
      auto failure = consensus_failure(
        request.task,
        agent_task_error_code::execution_failed,
        "consensus resolver failed with an unknown exception");
      failure.results = std::move(results);
      return failure;
    }
    if (!final_result) {
      consensus_result failure {
        .error_code = final_result.error_code == agent_task_error_code::none
                        ? agent_task_error_code::consensus_not_reached
                        : final_result.error_code,
        .error = final_result.error.empty()
                   ? "agents did not reach an exact-output consensus"
                   : final_result.error,
        .results = std::move(results),
        .final_result = std::move(final_result),
      };
      emit({
        .type = team_event_type::consensus_failed,
        .task_id = request.task.id,
        .session_id = request.task.session_id,
        .message = failure.error,
        .metadata = request.task.metadata,
      });
      session->update_task(request.task.id, agent_task_status::failed);
      return failure;
    }
    final_result.task_id = request.task.id;
    final_result.session_id = request.task.session_id;
    const auto invalid_artifact = std::find_if(
      final_result.artifacts.begin(), final_result.artifacts.end(), [](const auto& artifact) {
        return artifact.id.empty();
      });
    if (invalid_artifact != final_result.artifacts.end()) {
      auto failure = consensus_failure(
        request.task,
        agent_task_error_code::execution_failed,
        "consensus resolver returned an artifact without an id");
      failure.results = std::move(results);
      failure.final_result = std::move(final_result);
      return failure;
    }
    final_result.status = agent_task_status::completed;
    final_result.error_code = agent_task_error_code::none;
    final_result.error.clear();
    final_result.metadata["status"] = to_string(final_result.status);
    session->update_task(request.task.id, agent_task_status::completed);
    if (!final_result.output.empty()) {
      session->publish({
        .id = request.task.id + ":output",
        .role = agent_message_role::agent,
        .content = final_result.output,
      });
    }
    for (const auto& message : final_result.messages) {
      session->publish(message);
    }
    for (const auto& artifact : final_result.artifacts) {
      session->publish(artifact);
    }
    consensus_result output {
      .completed = true,
      .results = std::move(results),
      .final_result = std::move(final_result),
    };
    telemetry_errors += emit({
      .type = team_event_type::consensus_completed,
      .task_id = request.task.id,
      .session_id = request.task.session_id,
      .metadata = request.task.metadata,
    });
    if (telemetry_errors != 0) {
      output.final_result.metadata["telemetry_error_count"] =
        std::to_string(telemetry_errors);
    }
    return output;
  }

private:
  [[nodiscard]] std::optional<detail::team_runtime_slot> acquire_runtime_slot(
    std::stop_token stop_token,
    const std::optional<std::chrono::steady_clock::time_point>& deadline) const;

  [[nodiscard]] detail::agent_execution_outcome execute_agent(
    agent_lease lease,
    detail::team_runtime_slot runtime_slot,
    const agent_task_request& request,
    const std::shared_ptr<team_session>& session,
    std::stop_token external_stop_token,
    const std::optional<std::chrono::steady_clock::time_point>& deadline) const;

  [[nodiscard]] std::shared_ptr<team_session> get_or_create_session(
    const std::string& id) const {
    std::scoped_lock lock(state_->sessions_mutex);
    const auto found = state_->sessions.find(id);
    if (found != state_->sessions.end()) {
      return found->second;
    }
    auto session = std::make_shared<team_session>(id);
    state_->sessions.emplace(id, session);
    return session;
  }

  static std::chrono::milliseconds elapsed_since(
    std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  }

  agent_task_result finish_error(
    const agent_task_request& request,
    const std::shared_ptr<team_session>& session,
    std::chrono::steady_clock::time_point started,
    agent_task_status status,
    agent_task_error_code error_code,
    std::string error,
    std::size_t telemetry_errors) const {
    session->update_task(request.id, status);
    const auto type = status == agent_task_status::cancelled
                        ? team_event_type::task_cancelled
                        : status == agent_task_status::timed_out
                            ? team_event_type::task_timed_out
                            : team_event_type::task_blocked;
    telemetry_errors += emit({
      .type = type,
      .task_id = request.id,
      .session_id = request.session_id,
      .message = error,
      .elapsed = elapsed_since(started),
      .metadata = request.metadata,
    });
    agent_task_result result {
      .task_id = request.id,
      .session_id = request.session_id,
      .status = status,
      .error_code = error_code,
      .error = std::move(error),
      .elapsed = elapsed_since(started),
    };
    if (telemetry_errors != 0) {
      result.metadata["telemetry_error_count"] = std::to_string(telemetry_errors);
    }
    return result;
  }

  static agent_task_result exact_output_consensus(
    const std::vector<agent_task_result>& results,
    std::size_t minimum) {
    std::map<std::string, std::size_t> votes;
    for (const auto& result : results) {
      if (result) {
        ++votes[result.output];
      }
    }
    const auto best = std::max_element(votes.begin(), votes.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.second != rhs.second) {
        return lhs.second < rhs.second;
      }
      return lhs.first > rhs.first;
    });
    if (best == votes.end() || best->second < minimum) {
      return {
        .status = agent_task_status::failed,
        .error_code = agent_task_error_code::consensus_not_reached,
        .error = "agents did not produce enough matching outputs",
      };
    }
    return {
      .status = agent_task_status::completed,
      .output = best->first,
      .metadata = { { "matching_votes", std::to_string(best->second) } },
    };
  }

  consensus_result consensus_failure(
    const agent_task_request& task,
    agent_task_error_code code,
    std::string error,
    agent_task_status status = agent_task_status::failed) const {
    get_or_create_session(task.session_id)->update_task(task.id, status);
    emit({
      .type = status == agent_task_status::cancelled
                ? team_event_type::consensus_cancelled
                : team_event_type::consensus_failed,
      .task_id = task.id,
      .session_id = task.session_id,
      .message = error,
      .metadata = task.metadata,
    });
    return { .error_code = code, .error = std::move(error) };
  }

  std::size_t emit(const team_event& event) const {
    std::size_t failures = 0;
    const auto invoke = [&](const auto& callback) {
      try {
        callback();
      }
      catch (...) {
        if (state_->options.telemetry_failure_mode ==
            team_telemetry_failure_mode::propagate) {
          throw;
        }
        ++failures;
      }
    };
    if (state_->options.observer) {
      invoke([&] { state_->options.observer(event); });
    }
    if (state_->options.event_sink) {
      invoke([&] { state_->options.event_sink->publish({
        .module = "multi_agent",
        .name = to_string(event.type),
        .trace_id = event.metadata.contains("trace_id")
                      ? event.metadata.at("trace_id")
                      : std::string {},
        .subject_id = event.task_id,
        .elapsed = event.elapsed,
        .attributes = {
          { "session_id", event.session_id },
          { "agent_id", event.agent_id },
          { "event_type", to_string(event.type) },
        },
      }); });
    }
    return failures;
  }

  std::shared_ptr<detail::team_runtime_state> state_;
};

} // namespace wuwe::agent::multi_agent

#include <wuwe/agent/multi_agent/detail/team_runtime_execution.inl>

#endif // WUWE_AGENT_MULTI_AGENT_TEAM_RUNTIME_HPP
