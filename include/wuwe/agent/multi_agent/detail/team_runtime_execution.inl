#ifndef WUWE_AGENT_MULTI_AGENT_DETAIL_TEAM_RUNTIME_EXECUTION_INL
#define WUWE_AGENT_MULTI_AGENT_DETAIL_TEAM_RUNTIME_EXECUTION_INL

namespace wuwe::agent::multi_agent {

inline std::optional<detail::team_runtime_slot> team_runtime::acquire_runtime_slot(
  std::stop_token stop_token,
  const std::optional<std::chrono::steady_clock::time_point>& deadline) const {
  std::unique_lock lock(state_->concurrency_mutex);
  const auto available = [&] {
    return state_->active_runtime_tasks < state_->options.max_parallel_tasks;
  };
  const auto acquired =
    deadline ? state_->concurrency_condition.wait_until(lock, stop_token, *deadline, available)
             : state_->concurrency_condition.wait(lock, stop_token, available);
  if (!acquired)
    return std::nullopt;
  ++state_->active_runtime_tasks;
  return detail::team_runtime_slot(state_);
}

inline detail::agent_execution_outcome team_runtime::execute_agent(agent_lease lease,
  detail::team_runtime_slot runtime_slot, const agent_task_request& request,
  const std::shared_ptr<team_session>& session, std::stop_token external_stop_token,
  const std::optional<std::chrono::steady_clock::time_point>& deadline) const {
  if (deadline && std::chrono::steady_clock::now() >= *deadline) {
    return { .result = {
               .status = agent_task_status::timed_out,
               .error_code = agent_task_error_code::timed_out,
               .error = "agent task timed out before execution",
             } };
  }

  auto promise = std::make_shared<std::promise<agent_task_result>>();
  auto future = promise->get_future();
  auto stop_source = std::make_shared<std::stop_source>();
  std::optional<std::stop_callback<std::function<void()>>> external_stop;
  if (external_stop_token.stop_possible()) {
    external_stop.emplace(
      external_stop_token, std::function<void()>([stop_source] { stop_source->request_stop(); }));
  }
  try {
    std::thread worker([lease = std::move(lease),
                         runtime_slot = std::move(runtime_slot),
                         request,
                         session,
                         deadline,
                         stop_source,
                         promise]() mutable {
      try {
        auto result = lease.executor()->execute(request,
          {
            .session = session,
            .stop_token = stop_source->get_token(),
            .deadline = deadline,
          });

        // A ready future is the public completion boundary. Release both
        // capacity reservations before publishing completion so callers may
        // immediately submit follow-up work without observing a stale lease.
        lease = {};
        runtime_slot = {};
        promise->set_value(std::move(result));
      }
      catch (...) {
        const auto failure = std::current_exception();
        lease = {};
        runtime_slot = {};
        try {
          promise->set_exception(failure);
        }
        catch (...) {
          // Never allow promise bookkeeping to escape a detached worker.
        }
      }
    });
    try {
      worker.detach();
    }
    catch (...) {
      if (worker.joinable())
        worker.join();
    }
  }
  catch (const std::exception& ex) {
    return { .result = {
               .status = agent_task_status::failed,
               .error_code = agent_task_error_code::execution_failed,
               .error = std::string("failed to start agent executor: ") + ex.what(),
             } };
  }

  for (;;) {
    const auto wait_for = deadline ? (std::min)(state_->options.cancellation_poll_interval,
                                       std::chrono::duration_cast<std::chrono::milliseconds>(
                                         *deadline - std::chrono::steady_clock::now()))
                                   : state_->options.cancellation_poll_interval;
    if (future.wait_for((std::max)(wait_for, std::chrono::milliseconds { 0 })) ==
        std::future_status::ready) {
      try {
        return { .result = future.get() };
      }
      catch (const std::exception& ex) {
        return { .result = {
                   .status = agent_task_status::failed,
                   .error_code = agent_task_error_code::execution_failed,
                   .error = ex.what(),
                 } };
      }
      catch (...) {
        return { .result = {
                   .status = agent_task_status::failed,
                   .error_code = agent_task_error_code::execution_failed,
                   .error = "agent execution failed with an unknown exception",
                 } };
      }
    }
    if (external_stop_token.stop_requested()) {
      if (future.wait_for(std::chrono::milliseconds { 0 }) == std::future_status::ready)
        continue;
      stop_source->request_stop();
      return {
        .result = {
          .status = agent_task_status::cancelled,
          .error_code = agent_task_error_code::cancelled,
          .error = "agent task cancelled while executing",
        },
        .detached = true,
      };
    }
    if (deadline && std::chrono::steady_clock::now() >= *deadline) {
      if (future.wait_for(std::chrono::milliseconds { 0 }) == std::future_status::ready)
        continue;
      stop_source->request_stop();
      return {
        .result = {
          .status = agent_task_status::timed_out,
          .error_code = agent_task_error_code::timed_out,
          .error = "agent task execution timed out",
        },
        .detached = true,
      };
    }
  }
}

} // namespace wuwe::agent::multi_agent

#endif // WUWE_AGENT_MULTI_AGENT_DETAIL_TEAM_RUNTIME_EXECUTION_INL
