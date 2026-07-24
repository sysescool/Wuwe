#ifndef WUWE_AGENT_EVALUATION_BUILTIN_EVALUATORS_HPP
#define WUWE_AGENT_EVALUATION_BUILTIN_EVALUATORS_HPP

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wuwe/agent/evaluation/evaluation_core.hpp>

namespace wuwe::agent::evaluation {

struct text_match_options {
  bool case_sensitive { false };
  bool trim_whitespace { true };
};

class exact_match_evaluator final : public evaluator {
public:
  explicit exact_match_evaluator(text_match_options options = {}) : options_(options) {
  }

  [[nodiscard]] std::string name() const override {
    return "exact_match";
  }

  evaluation_metric_result evaluate(const evaluation_case& value) const override {
    const auto actual = normalize(value.output);
    const auto expected = normalize(value.expected_output);
    const bool passed = actual == expected;
    return {
      .name = name(),
      .score = passed ? 1.0 : 0.0,
      .passed = passed,
      .explanation = passed ? "output exactly matched the expected value"
                            : "output did not match the expected value",
    };
  }

private:
  [[nodiscard]] std::string normalize(std::string value) const {
    if (options_.trim_whitespace) {
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
      }
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
      }
    }
    if (!options_.case_sensitive) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
    }
    return value;
  }

  text_match_options options_;
};

struct contains_terms_options {
  std::vector<std::string> terms;
  bool require_all { true };
  bool case_sensitive { false };
};

class contains_terms_evaluator final : public evaluator {
public:
  explicit contains_terms_evaluator(contains_terms_options options)
      : options_(std::move(options)) {
  }

  [[nodiscard]] std::string name() const override {
    return "contains_terms";
  }

  evaluation_metric_result evaluate(const evaluation_case& value) const override {
    std::size_t matches = 0;
    std::vector<std::string> missing;
    for (const auto& term : options_.terms) {
      if (contains(value.output, term)) {
        ++matches;
      }
      else {
        missing.push_back(term);
      }
    }
    const double score = options_.terms.empty()
                           ? 1.0
                           : static_cast<double>(matches) /
                               static_cast<double>(options_.terms.size());
    const bool passed = options_.terms.empty() ||
                        (options_.require_all ? matches == options_.terms.size() : matches != 0);
    return {
      .name = name(),
      .score = score,
      .passed = passed,
      .explanation = passed ? "output contained the required terms"
                            : "output was missing required terms",
      .evidence = std::move(missing),
    };
  }

private:
  [[nodiscard]] bool contains(std::string_view value, std::string_view term) const {
    if (options_.case_sensitive) {
      return value.find(term) != std::string::npos;
    }
    if (term.empty()) {
      return true;
    }
    for (std::size_t index = 0; index + term.size() <= value.size(); ++index) {
      bool matches = true;
      for (std::size_t character = 0; character < term.size(); ++character) {
        if (std::tolower(static_cast<unsigned char>(value[index + character])) !=
            std::tolower(static_cast<unsigned char>(term[character]))) {
          matches = false;
          break;
        }
      }
      if (matches) {
        return true;
      }
    }
    return false;
  }

  contains_terms_options options_;
};

struct trajectory_expectation {
  std::vector<std::string> required_events;
  std::vector<std::string> required_sequence;
  std::vector<std::string> forbidden_events;
  std::map<std::string, std::size_t> maximum_occurrences;
};

class trajectory_evaluator final : public evaluator {
public:
  explicit trajectory_evaluator(trajectory_expectation expectation)
      : expectation_(std::move(expectation)) {
  }

  [[nodiscard]] std::string name() const override {
    return "trajectory";
  }

  evaluation_metric_result evaluate(const evaluation_case& value) const override {
    if (!value.trajectory.is_array()) {
      return {
        .name = name(),
        .score = 0.0,
        .passed = false,
        .explanation = "trajectory must be a JSON array",
      };
    }
    std::map<std::string, std::size_t> counts;
    std::vector<std::string> events;
    for (const auto& event : value.trajectory) {
      if (event.is_string()) {
        const auto type = event.get<std::string>();
        ++counts[type];
        events.push_back(type);
      }
      else if (event.is_object()) {
        const auto type = event.value("type", std::string {});
        if (!type.empty()) {
          ++counts[type];
          events.push_back(type);
        }
      }
    }

    std::vector<std::string> violations;
    std::size_t satisfied = 0;
    std::size_t expectations = expectation_.required_events.size() +
                               expectation_.forbidden_events.size() +
                               expectation_.maximum_occurrences.size() +
                               (expectation_.required_sequence.empty() ? 0U : 1U);
    for (const auto& event : expectation_.required_events) {
      if (counts[event] != 0) {
        ++satisfied;
      }
      else {
        violations.push_back("missing:" + event);
      }
    }
    for (const auto& event : expectation_.forbidden_events) {
      if (counts[event] == 0) {
        ++satisfied;
      }
      else {
        violations.push_back("forbidden:" + event);
      }
    }
    if (!expectation_.required_sequence.empty()) {
      std::size_t next = 0;
      for (const auto& event : events) {
        if (next < expectation_.required_sequence.size() &&
            event == expectation_.required_sequence[next]) {
          ++next;
        }
      }
      if (next == expectation_.required_sequence.size()) {
        ++satisfied;
      }
      else {
        violations.push_back("sequence");
      }
    }
    for (const auto& [event, maximum] : expectation_.maximum_occurrences) {
      if (counts[event] <= maximum) {
        ++satisfied;
      }
      else {
        violations.push_back("too_many:" + event);
      }
    }
    const double score = expectations == 0
                           ? 1.0
                           : static_cast<double>(satisfied) /
                               static_cast<double>(expectations);
    return {
      .name = name(),
      .score = score,
      .passed = violations.empty(),
      .explanation = violations.empty() ? "trajectory satisfied all expectations"
                                        : "trajectory violated expectations",
      .evidence = std::move(violations),
    };
  }

private:
  trajectory_expectation expectation_;
};

} // namespace wuwe::agent::evaluation

#endif // WUWE_AGENT_EVALUATION_BUILTIN_EVALUATORS_HPP
