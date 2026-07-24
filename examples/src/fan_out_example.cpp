#include "console_utf8.hpp"

#include <string>
#include <vector>

#include <wuwe/agent/orchestration/orchestration.hpp>
#include <wuwe/common/print.h>

int main() {
  wuwe_example::configure_utf8_console();

  auto analyze = wuwe::fan_out(
    wuwe::fan_out_options { .max_concurrency = 2 },
    [](const std::string& text) {
      return "length=" + std::to_string(text.size());
    },
    [](const std::string& text) {
      return "first=" + std::string(1, text.front());
    },
    [](const std::string& text) {
      return "last=" + std::string(1, text.back());
    });

  auto result = analyze.run("Wuwe");
  auto facts = wuwe::fan_in_all()(std::move(result));
  for (const auto& fact : facts) {
    wuwe::println("{}", fact);
  }

  auto parallel_map = wuwe::fan_out_each(
    wuwe::fan_out_options { .max_concurrency = 2 },
    [](const int& value) { return value * value; });
  const auto squares = wuwe::fan_in_all()(
    parallel_map.run(std::vector<int> { 1, 2, 3, 4 }));
  for (const auto value : squares) {
    wuwe::println("square={}", value);
  }
}
