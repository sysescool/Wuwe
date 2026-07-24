#ifndef WUWE_AGENT_GUARDRAILS_GUARDRAIL_PIPELINE_HPP
#define WUWE_AGENT_GUARDRAILS_GUARDRAIL_PIPELINE_HPP

#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/audit/audit_sink.hpp>
#include <wuwe/agent/core/observability.hpp>
#include <wuwe/agent/guardrails/guardrail_core.hpp>

namespace wuwe::agent::guardrails {

enum class guardrail_failure_mode {
  closed,
  open,
};

using guardrail_telemetry_failure_mode = observability::telemetry_failure_mode;

struct guardrail_pipeline_options {
  guardrail_failure_mode failure_mode { guardrail_failure_mode::closed };
  guardrail_observer observer;
  audit::audit_sink* audit_sink {};
  observability::event_sink* event_sink {};
  guardrail_telemetry_failure_mode telemetry_failure_mode {
    guardrail_telemetry_failure_mode::ignore
  };
};

class guardrail_pipeline {
public:
  explicit guardrail_pipeline(guardrail_pipeline_options options = {})
      : options_(std::move(options)) {
  }

  guardrail_pipeline& add(std::shared_ptr<guardrail> value) {
    if (!value) {
      throw std::invalid_argument("guardrail_pipeline cannot add a null guardrail");
    }
    guardrails_.push_back(std::move(value));
    return *this;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return guardrails_.size();
  }

  [[nodiscard]] guardrail_run_result evaluate(guardrail_request request) const {
    const auto started = std::chrono::steady_clock::now();
    guardrail_run_result run {
      .stage = request.stage,
      .subject_id = request.subject_id,
      .decision = guardrail_decision::allow,
      .content = request.content,
      .data = request.data,
      .metadata = request.metadata,
    };

    for (const auto& value : guardrails_) {
      const auto check_started = std::chrono::steady_clock::now();
      guardrail_check_result check;
      try {
        check.guardrail_name = value->name();
        request.content = run.content;
        request.data = run.data;
        check.result = value->evaluate(request);
      }
      catch (const std::exception& ex) {
        if (check.guardrail_name.empty()) {
          check.guardrail_name = "unnamed_guardrail";
        }
        check.error = ex.what();
        check.result = failure_result(check.guardrail_name, ex.what());
      }
      catch (...) {
        if (check.guardrail_name.empty()) {
          check.guardrail_name = "unnamed_guardrail";
        }
        check.error = "unknown guardrail exception";
        check.result = failure_result(check.guardrail_name, check.error);
      }
      check.elapsed = elapsed_since(check_started);

      for (const auto& issue : check.result.issues) {
        run.issues.push_back(issue);
      }
      for (const auto& [key, metadata_value] : check.result.metadata) {
        run.metadata[check.guardrail_name + "." + key] = metadata_value;
      }
      apply_result(run, check.result);
      run.checks.push_back(std::move(check));

      if (run.decision == guardrail_decision::deny ||
          run.decision == guardrail_decision::require_approval) {
        break;
      }
    }

    run.elapsed = elapsed_since(started);
    publish(run);
    return run;
  }

  [[nodiscard]] guardrail_run_result evaluate(
    guardrail_stage stage,
    std::string content,
    std::string subject_id = {},
    std::map<std::string, std::string> metadata = {}) const {
    return evaluate({
      .stage = stage,
      .subject_id = std::move(subject_id),
      .content = std::move(content),
      .metadata = std::move(metadata),
    });
  }

private:
  [[nodiscard]] guardrail_result failure_result(
    const std::string& name,
    const std::string& message) const {
    if (options_.failure_mode == guardrail_failure_mode::open) {
      auto result = guardrail_result::allow();
      result.issues.push_back({
        .severity = guardrail_severity::error,
        .code = "guardrail_error_ignored",
        .message = "guardrail '" + name + "' failed open: " + message,
      });
      return result;
    }
    return guardrail_result::deny({
      .severity = guardrail_severity::critical,
      .code = "guardrail_error",
      .message = "guardrail '" + name + "' failed: " + message,
    });
  }

  static void apply_result(guardrail_run_result& run, const guardrail_result& result) {
    if (result.replacement_content) {
      run.content = *result.replacement_content;
    }
    if (result.replacement_data) {
      run.data = *result.replacement_data;
    }
    if (result.decision == guardrail_decision::deny ||
        result.decision == guardrail_decision::require_approval) {
      run.decision = result.decision;
    }
    else if ((result.decision == guardrail_decision::modify ||
              result.replacement_content || result.replacement_data) &&
             run.decision == guardrail_decision::allow) {
      run.decision = guardrail_decision::modify;
    }
  }

  void publish(guardrail_run_result& run) const {
    std::size_t failures = 0;
    auto invoke = [&](const auto& callback) {
      try {
        callback();
      }
      catch (...) {
        if (options_.telemetry_failure_mode == guardrail_telemetry_failure_mode::propagate) {
          throw;
        }
        ++failures;
      }
    };
    if (options_.observer) {
      invoke([&] { options_.observer(run); });
    }
    if (options_.audit_sink) {
      invoke([&] { options_.audit_sink->publish({
        .module = "guardrails",
        .name = to_string(run.stage),
        .subject_id = run.subject_id,
        .outcome = run.allowed() ? audit::audit_event_outcome::allowed
                                 : audit::audit_event_outcome::denied,
        .elapsed = run.elapsed,
        .attributes = {
          { "decision", to_string(run.decision) },
          { "checks", std::to_string(run.checks.size()) },
          { "issues", std::to_string(run.issues.size()) },
        },
      }); });
    }
    if (options_.event_sink) {
      invoke([&] { options_.event_sink->publish({
        .module = "guardrails",
        .name = "guardrail_evaluated",
        .subject_id = run.subject_id,
        .elapsed = run.elapsed,
        .attributes = {
          { "stage", to_string(run.stage) },
          { "decision", to_string(run.decision) },
          { "checks", std::to_string(run.checks.size()) },
          { "issues", std::to_string(run.issues.size()) },
        },
      }); });
    }
    if (failures != 0) {
      run.metadata["telemetry_error_count"] = std::to_string(failures);
    }
  }

  static std::chrono::milliseconds elapsed_since(
    std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  }

  guardrail_pipeline_options options_;
  std::vector<std::shared_ptr<guardrail>> guardrails_;
};

} // namespace wuwe::agent::guardrails

#endif // WUWE_AGENT_GUARDRAILS_GUARDRAIL_PIPELINE_HPP
