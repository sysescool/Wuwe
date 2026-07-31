#ifndef WUWE_AGENT_LEARNING_ADAPTATION_CORE_HPP
#define WUWE_AGENT_LEARNING_ADAPTATION_CORE_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace wuwe::agent::learning {

enum class feedback_kind {
  positive,
  negative,
  correction,
  preference,
  outcome,
};

[[nodiscard]] inline std::string to_string(feedback_kind value) {
  switch (value) {
    case feedback_kind::positive:
      return "positive";
    case feedback_kind::negative:
      return "negative";
    case feedback_kind::correction:
      return "correction";
    case feedback_kind::preference:
      return "preference";
    case feedback_kind::outcome:
      return "outcome";
  }
  return "unknown";
}

struct experience_record {
  std::string id;
  std::string target;
  std::string source;
  std::string source_run_id;
  std::string input;
  std::string output;
  std::string expected_output;
  feedback_kind feedback_type { feedback_kind::outcome };
  nlohmann::json feedback = nlohmann::json::object();
  nlohmann::json trajectory;
  std::chrono::system_clock::time_point created_at { std::chrono::system_clock::now() };
  std::map<std::string, std::string> metadata;
};

struct experience_query {
  std::string target;
  std::string source;
  std::optional<std::chrono::system_clock::time_point> since;
  std::size_t limit { 100 };
  std::map<std::string, std::string> filters;
};

struct reward_record {
  std::string id;
  std::string experience_id;
  std::string target;
  std::string objective;
  double value { 0.0 };
  double weight { 1.0 };
  std::map<std::string, double> components;
  std::string source;
  std::chrono::system_clock::time_point created_at { std::chrono::system_clock::now() };
  std::map<std::string, std::string> metadata;
};

struct reward_query {
  std::string target;
  std::string experience_id;
  std::string objective;
  std::optional<std::chrono::system_clock::time_point> since;
  std::size_t limit { 100 };
  std::map<std::string, std::string> filters;
};

inline std::string make_adaptation_id(const char* prefix) {
  static std::atomic<std::uint64_t> next { 1 };
  const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::system_clock::now().time_since_epoch())
                     .count();
  return std::string(prefix) + "-" + std::to_string(now) + "-" +
         std::to_string(next.fetch_add(1, std::memory_order_relaxed));
}

inline nlohmann::json experience_record_to_json(const experience_record& value) {
  return {
    { "id", value.id },
    { "target", value.target },
    { "source", value.source },
    { "source_run_id", value.source_run_id },
    { "input", value.input },
    { "output", value.output },
    { "expected_output", value.expected_output },
    { "feedback_type", to_string(value.feedback_type) },
    { "feedback", value.feedback },
    { "trajectory", value.trajectory },
    { "metadata", value.metadata },
  };
}

inline nlohmann::json reward_record_to_json(const reward_record& value) {
  return {
    { "id", value.id },
    { "experience_id", value.experience_id },
    { "target", value.target },
    { "objective", value.objective },
    { "value", value.value },
    { "weight", value.weight },
    { "components", value.components },
    { "source", value.source },
    { "metadata", value.metadata },
  };
}

} // namespace wuwe::agent::learning

#endif // WUWE_AGENT_LEARNING_ADAPTATION_CORE_HPP
