#include <string>

#include "console_utf8.hpp"

#include <wuwe/agent/reasoning/reasoning.hpp>
#include <wuwe/common/print.h>

int main() {
  wuwe_example::configure_utf8_console();
  namespace reasoning = wuwe::agent::reasoning;

  reasoning::best_of_n_runner runner({
    .generator = [](const reasoning::reasoning_request& request,
                   const reasoning::best_of_n_context&) {
      reasoning::reasoning_result result;
      result.completed = true;
      result.content = request.metadata.at("candidate");
      result.final_response.content = result.content;
      result.usage.model_calls = 1;
      result.usage.total_tokens = result.content.size();
      return result;
    },
    .scorer = [](const reasoning::reasoning_request&,
                const reasoning::reasoning_result& result,
                const reasoning::best_of_n_context&) {
      return reasoning::best_of_n_score {
        .value = static_cast<double>(result.content.size()),
        .accepted = !result.content.empty(),
        .rationale = "prefer the most complete candidate",
      };
    },
    .request_builder = [](const reasoning::reasoning_request& base, std::size_t index) {
      auto request = base;
      const std::string candidates[] {
        "Concise answer.",
        "Clear answer with supporting detail.",
        "Answer with detail, trade-offs, and a concrete recommendation.",
      };
      request.metadata["candidate"] = candidates[index];
      return request;
    },
  });

  const auto result = runner.run({ .input = "Provide a useful answer." }, {
    .policy = {
      .candidate_count = 3,
      .max_concurrency = 2,
    },
  });
  if (!result) {
    wuwe::println("best-of-n failed: {}", result.error);
    return 1;
  }

  const auto* selected = result.selected_candidate();
  wuwe::println("selected candidate {} with score {}",
    selected->index,
    selected->score->value);
  wuwe::println("{}", selected->result.content);
  wuwe::println("aggregate model calls: {}", result.aggregate_usage.model_calls);
}
