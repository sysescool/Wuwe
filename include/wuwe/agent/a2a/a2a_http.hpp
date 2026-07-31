#ifndef WUWE_AGENT_A2A_A2A_HTTP_HPP
#define WUWE_AGENT_A2A_A2A_HTTP_HPP

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <wuwe/agent/a2a/a2a_service.hpp>
#include <wuwe/net/default_http_client.h>
#include <wuwe/net/http_client.h>

namespace wuwe::agent::a2a {

struct http_client_transport_options {
  std::string endpoint;
  std::string agent_card_url;
  std::vector<std::pair<std::string, std::string>> headers;
  http_timeout_options timeouts;
  bool follow_redirects { true };
};

class http_client_transport final : public transport {
public:
  explicit http_client_transport(http_client_transport_options options,
    std::shared_ptr<::wuwe::http_client> http = std::make_shared<::wuwe::default_http_client>())
      : options_(std::move(options)), http_(std::move(http)) {
    if (options_.endpoint.empty()) {
      throw std::invalid_argument("A2A HTTP transport requires an endpoint");
    }
    if (options_.agent_card_url.empty()) {
      options_.agent_card_url = default_agent_card_url(options_.endpoint);
    }
    if (!http_) {
      throw std::invalid_argument("A2A HTTP transport requires an HTTP client");
    }
  }

  rpc_result invoke(
    std::string method, nlohmann::json params, std::stop_token stop_token = {}) override {
    if (stop_token.stop_requested()) {
      return cancelled_result("A2A request cancelled before transport");
    }
    const auto id = ++sequence_;
    auto headers = options_.headers;
    ensure_header(headers, "Content-Type", "application/json");
    ensure_header(headers, "Accept", "application/json");
    const auto response = http_->send({
      .method = "POST",
      .url = options_.endpoint,
      .headers = std::move(headers),
      .body =
        nlohmann::json {
          { "jsonrpc", "2.0" },
          { "id", id },
          { "method", std::move(method) },
          { "params", std::move(params) },
        }
          .dump(),
      .timeouts = options_.timeouts,
      .follow_redirects = options_.follow_redirects,
    });
    if (response.error_code) {
      return { .failure = error {
                 .code = error_code::transport_error,
                 .message = response.error_code.message(),
               } };
    }
    if (stop_token.stop_requested()) {
      return cancelled_result("A2A request cancelled after transport");
    }
    if (response.status_code < 200 || response.status_code >= 300) {
      return { .failure = error {
                 .code = error_code::transport_error,
                 .message =
                   "A2A HTTP request failed with status " + std::to_string(response.status_code),
                 .data = response.body.empty() ? std::nullopt
                                               : std::optional<nlohmann::json>(response.body),
               } };
    }
    try {
      const auto body = nlohmann::json::parse(response.body);
      if (body.value("jsonrpc", "") != "2.0" || !body.contains("id") || body.at("id") != id) {
        throw std::invalid_argument("A2A JSON-RPC response id does not match the request");
      }
      const auto has_error = body.contains("error");
      const auto has_result = body.contains("result");
      if (has_error == has_result) {
        throw std::invalid_argument(
          "A2A JSON-RPC response must contain exactly one of result or error");
      }
      if (has_error) {
        const auto failure = body.at("error");
        if (!failure.is_object() || !failure.contains("code") ||
            !failure.at("code").is_number_integer() || !failure.contains("message") ||
            !failure.at("message").is_string()) {
          throw std::invalid_argument("A2A JSON-RPC error response is malformed");
        }
        return { .failure = error {
                   .code = static_cast<error_code>(failure.at("code").get<int>()),
                   .message = failure.at("message").get<std::string>(),
                   .data = failure.contains("data")
                             ? std::optional<nlohmann::json>(failure.at("data"))
                             : std::nullopt,
                 } };
      }
      return { .value = body.at("result") };
    }
    catch (const std::exception& ex) {
      return { .failure = error {
                 .code = error_code::invalid_agent_response,
                 .message = ex.what(),
               } };
    }
  }

  result<agent_card> discover(std::stop_token stop_token = {}) override {
    if (stop_token.stop_requested()) {
      return { .failure = error {
                 .code = error_code::transport_error,
                 .message = "A2A discovery cancelled before transport",
               } };
    }
    auto headers = options_.headers;
    ensure_header(headers, "Accept", "application/json");
    const auto response = http_->send({
      .method = "GET",
      .url = options_.agent_card_url,
      .headers = std::move(headers),
      .timeouts = options_.timeouts,
      .follow_redirects = options_.follow_redirects,
    });
    if (response.error_code || response.status_code < 200 || response.status_code >= 300) {
      return { .failure = error {
                 .code = error_code::transport_error,
                 .message = response.error_code ? response.error_code.message()
                                                : "A2A Agent Card request failed with status " +
                                                    std::to_string(response.status_code),
               } };
    }
    if (stop_token.stop_requested()) {
      return { .failure = error {
                 .code = error_code::transport_error,
                 .message = "A2A discovery cancelled after transport",
               } };
    }
    try {
      return { .value = agent_card_from_json(nlohmann::json::parse(response.body)) };
    }
    catch (const std::exception& ex) {
      return { .failure = error {
                 .code = error_code::invalid_agent_response,
                 .message = ex.what(),
               } };
    }
  }

private:
  static rpc_result cancelled_result(std::string message) {
    return { .failure = error {
               .code = error_code::transport_error,
               .message = std::move(message),
             } };
  }

  static std::string default_agent_card_url(const std::string& endpoint) {
    const auto scheme = endpoint.find("://");
    if (scheme == std::string::npos) {
      return endpoint + "/.well-known/agent-card.json";
    }
    const auto path = endpoint.find_first_of("/?#", scheme + 3);
    const auto origin = path == std::string::npos ? endpoint : endpoint.substr(0, path);
    return origin + "/.well-known/agent-card.json";
  }

  static void ensure_header(std::vector<std::pair<std::string, std::string>>& headers,
    std::string name, std::string value) {
    const auto found = std::find_if(headers.begin(), headers.end(), [&](const auto& header) {
      return ::wuwe::http_header_name_equals(header.first, name);
    });
    if (found == headers.end()) {
      headers.push_back({ std::move(name), std::move(value) });
    }
  }

  http_client_transport_options options_;
  std::shared_ptr<::wuwe::http_client> http_;
  std::atomic<std::uint64_t> sequence_ { 0 };
};

struct http_service_request {
  std::string method;
  std::string path;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

struct http_service_response {
  int status_code { 200 };
  std::string content_type { "application/json" };
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

class http_service_adapter {
public:
  explicit http_service_adapter(std::shared_ptr<service> service) : service_(std::move(service)) {
    if (!service_) {
      throw std::invalid_argument("A2A HTTP service adapter requires a service");
    }
  }

  http_service_response handle(
    const http_service_request& request, std::stop_token stop_token = {}) const {
    if (request.method == "GET" && (request.path == "/.well-known/agent-card.json" ||
                                     request.path == "/.well-known/agent.json")) {
      return json_response(200, to_json(service_->card()));
    }
    if (request.method != "POST") {
      auto response = json_response(405, { { "error", "method not allowed" } });
      response.headers.push_back({ "Allow", "GET, POST" });
      return response;
    }
    if (!is_json_content_type(request.headers)) {
      return json_response(415, { { "error", "content-type must be application/json" } });
    }
    try {
      auto response = service_->handle_jsonrpc(nlohmann::json::parse(request.body), stop_token);
      if (response.is_null()) {
        return {
          .status_code = 204,
          .content_type = {},
          .body = {},
        };
      }
      return json_response(200, std::move(response));
    }
    catch (const std::exception& ex) {
      return json_response(400,
        {
          { "jsonrpc", "2.0" },
          { "id", nullptr },
          { "error",
            {
              { "code", static_cast<int>(error_code::parse_error) },
              { "message", ex.what() },
            } },
        });
    }
  }

private:
  static http_service_response json_response(int status, nlohmann::json body) {
    return {
      .status_code = status,
      .body = std::move(body).dump(),
      .headers = { { "Content-Type", "application/json" } },
    };
  }

  static bool is_json_content_type(
    const std::vector<std::pair<std::string, std::string>>& headers) {
    for (const auto& [name, value] : headers) {
      if (::wuwe::http_header_name_equals(name, "Content-Type")) {
        constexpr std::string_view expected = "application/json";
        return std::search(value.begin(),
                 value.end(),
                 expected.begin(),
                 expected.end(),
                 [](char lhs, char rhs) {
                   return std::tolower(static_cast<unsigned char>(lhs)) ==
                          std::tolower(static_cast<unsigned char>(rhs));
                 }) != value.end();
      }
    }
    return true;
  }

  std::shared_ptr<service> service_;
};

} // namespace wuwe::agent::a2a

#endif // WUWE_AGENT_A2A_A2A_HTTP_HPP
