#ifndef WUWE_AGENT_LLM_RETRY_HPP
#define WUWE_AGENT_LLM_RETRY_HPP

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <random>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <wuwe/agent/llm/llm_config.h>
#include <wuwe/agent/llm/llm_error.h>
#include <wuwe/net/http_client.h>
#include <wuwe/net/net_errc.h>

WUWE_NAMESPACE_BEGIN

namespace agent::llm_detail {

inline bool is_retryable_error(const std::error_code& ec) {
  return ec == llm_error_code::rate_limited || ec == llm_error_code::timeout ||
         ec == net_errc::rate_limited || ec == net_errc::timeout ||
         ec == net_errc::connection_failed || ec == net_errc::transport_failed ||
         ec == net_errc::server_error || ec == net_errc::service_unavailable;
}

inline int compute_backoff_ms(
  int attempt,
  int base_backoff_ms,
  int max_backoff_ms = 30000,
  double jitter_ratio = 0.0) {
  constexpr int max_power = 6;
  const int clamped_attempt = attempt < max_power ? attempt : max_power;
  const auto base = (std::max)(1, base_backoff_ms);
  const auto maximum = (std::max)(base, max_backoff_ms);
  const auto unbounded = static_cast<long long>(base) * (1LL << clamped_attempt);
  double delay = static_cast<double>((std::min)(
    unbounded, static_cast<long long>(maximum)));
  if (std::isfinite(jitter_ratio) && jitter_ratio > 0.0) {
    thread_local std::mt19937_64 source(std::random_device {}());
    std::uniform_real_distribution<double> distribution(-1.0, 1.0);
    delay *= 1.0 + distribution(source) * (std::min)(jitter_ratio, 1.0);
  }
  delay = (std::clamp)(delay, 0.0, static_cast<double>(maximum));
  return static_cast<int>(delay);
}

inline std::optional<std::chrono::milliseconds> parse_retry_after(
  const http_response& response,
  std::chrono::system_clock::time_point now =
    std::chrono::system_clock::now()) {
  const auto header = find_http_header(response.headers, "Retry-After");
  if (!header) {
    return std::nullopt;
  }
  auto value = *header;
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }

  long long seconds {};
  const auto parsed = std::from_chars(
    value.data(), value.data() + value.size(), seconds);
  if (parsed.ec == std::errc {} && parsed.ptr == value.data() + value.size() &&
      seconds >= 0) {
    const auto maximum_seconds =
      (std::numeric_limits<long long>::max)() / 1000;
    return std::chrono::milliseconds(
      (std::min)(seconds, maximum_seconds) * 1000);
  }

  std::tm parsed_time {};
  std::istringstream input { std::string(value) };
  input.imbue(std::locale::classic());
  input >> std::get_time(&parsed_time, "%a, %d %b %Y %H:%M:%S GMT");
  if (input.fail()) {
    return std::nullopt;
  }
#if defined(_WIN32)
  const auto target = _mkgmtime(&parsed_time);
#else
  const auto target = timegm(&parsed_time);
#endif
  if (target < 0) {
    return std::nullopt;
  }
  const auto target_time = std::chrono::system_clock::from_time_t(target);
  if (target_time <= now) {
    return std::chrono::milliseconds::zero();
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(target_time - now);
}

inline std::chrono::milliseconds compute_retry_delay(
  const llm_client_config& config,
  int attempt,
  const http_response& response) {
  auto delay = std::chrono::milliseconds(compute_backoff_ms(
    attempt,
    config.retry_backoff_ms <= 0 ? 500 : config.retry_backoff_ms,
    config.retry_max_backoff_ms <= 0 ? 30000 : config.retry_max_backoff_ms,
    config.retry_jitter_ratio));
  if (!config.respect_retry_after) {
    return delay;
  }
  if (const auto server_delay = parse_retry_after(response)) {
    const auto cap = std::chrono::milliseconds(
      config.retry_max_server_delay_ms <= 0
        ? 60000
        : config.retry_max_server_delay_ms);
    delay = (std::max)(delay, (std::min)(*server_delay, cap));
  }
  return delay;
}

inline void apply_retry_metadata(
  llm_response& result,
  const http_response& response) {
  if (const auto delay = parse_retry_after(response)) {
    result.metadata["retry_after_ms"] = std::to_string(delay->count());
  }
}

inline bool wait_for_retry(std::stop_token stop_token, std::chrono::milliseconds duration) {
  constexpr auto poll_interval = std::chrono::milliseconds(50);
  auto remaining = duration;
  while (remaining.count() > 0) {
    if (stop_token.stop_requested()) {
      return false;
    }
    const auto sleep_time = (std::min)(remaining, poll_interval);
    std::this_thread::sleep_for(sleep_time);
    remaining -= sleep_time;
  }
  return !stop_token.stop_requested();
}

} // namespace agent::llm_detail

WUWE_NAMESPACE_END

#endif // WUWE_AGENT_LLM_RETRY_HPP
