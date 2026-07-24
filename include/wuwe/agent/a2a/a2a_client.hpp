#ifndef WUWE_AGENT_A2A_A2A_CLIENT_HPP
#define WUWE_AGENT_A2A_A2A_CLIENT_HPP

#include <memory>
#include <stop_token>
#include <string>
#include <utility>

#include <wuwe/agent/a2a/a2a_transport.hpp>

namespace wuwe::agent::a2a {

class client {
public:
  explicit client(std::shared_ptr<transport> transport)
      : transport_(std::move(transport)) {
    if (!transport_) {
      throw std::invalid_argument("A2A client requires a transport");
    }
  }

  result<agent_card> discover(std::stop_token stop_token = {}) const {
    return transport_->discover(stop_token);
  }

  [[nodiscard]] transport_capabilities capabilities() const noexcept {
    return transport_->capabilities();
  }

  result<task> send(
    const send_message_params& params,
    std::stop_token stop_token = {}) const {
    nlohmann::json configuration {
      { "acceptedOutputModes", params.configuration.accepted_output_modes },
      { "historyLength", params.configuration.history_length },
      { "blocking", params.configuration.blocking },
    };
    return invoke_task("message/send", {
      { "message", to_json(params.value) },
      { "configuration", std::move(configuration) },
      { "metadata", params.metadata },
    }, stop_token);
  }

  result<task> get(
    const task_query_params& params,
    std::stop_token stop_token = {}) const {
    return invoke_task("tasks/get", {
      { "id", params.id },
      { "historyLength", params.history_length },
      { "metadata", params.metadata },
    }, stop_token);
  }

  result<task> cancel(
    const task_id_params& params,
    std::stop_token stop_token = {}) const {
    return invoke_task("tasks/cancel", {
      { "id", params.id },
      { "metadata", params.metadata },
    }, stop_token);
  }

private:
  result<task> invoke_task(
    std::string method,
    nlohmann::json params,
    std::stop_token stop_token) const {
    auto response = transport_->invoke(std::move(method), std::move(params), stop_token);
    if (!response) {
      return { .failure = std::move(response.failure) };
    }
    try {
      return { .value = task_from_json(response.value) };
    }
    catch (const std::exception& ex) {
      return { .failure = error {
        .code = error_code::invalid_agent_response,
        .message = ex.what(),
      } };
    }
  }

  std::shared_ptr<transport> transport_;
};

} // namespace wuwe::agent::a2a

#endif // WUWE_AGENT_A2A_A2A_CLIENT_HPP
