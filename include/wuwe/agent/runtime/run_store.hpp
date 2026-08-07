#ifndef WUWE_AGENT_RUNTIME_RUN_STORE_HPP
#define WUWE_AGENT_RUNTIME_RUN_STORE_HPP

#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <wuwe/agent/core/storage.hpp>
#include <wuwe/agent/runtime/run_types.hpp>

namespace wuwe::agent::runtime {

enum class run_store_write_status {
  applied,
  already_exists,
  not_found,
  conflict,
};

using run_store_coordination_scope = core::storage_coordination_scope;
using agent_run_store_capabilities = core::storage_capabilities;

inline void validate_agent_run_store_capabilities(
  const agent_run_store_capabilities& capabilities) {
  core::validate_storage_capabilities(capabilities);
  if (capabilities.declared && (!capabilities.atomic_mutations || !capabilities.ordered_replay)) {
    throw std::invalid_argument(
      "agent run store requires atomic mutations and ordered event replay");
  }
}

struct run_store_write_result {
  run_store_write_status status { run_store_write_status::applied };
  std::uint64_t revision { 0 };

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == run_store_write_status::applied;
  }
};

class agent_run_store {
public:
  virtual ~agent_run_store() = default;

  [[nodiscard]] virtual agent_run_store_capabilities capabilities() const noexcept {
    return {};
  }

  virtual run_store_write_result create(agent_run_record record, agent_run_event event) = 0;
  [[nodiscard]] virtual std::optional<agent_run_record> load(const std::string& run_id) const = 0;
  virtual run_store_write_result update(
    std::uint64_t expected_revision, agent_run_record record, agent_run_event event) = 0;
  [[nodiscard]] virtual std::vector<agent_run_event> list_events(
    const std::string& run_id, std::uint64_t after_sequence = 0) const = 0;
};

class in_memory_agent_run_store final : public agent_run_store {
public:
  [[nodiscard]] agent_run_store_capabilities capabilities() const noexcept override {
    return {
      .declared = true,
      .optimistic_concurrency = true,
      .atomic_mutations = true,
      .ordered_replay = true,
      .coordination_scope = run_store_coordination_scope::process_local,
    };
  }

  run_store_write_result create(agent_run_record record, agent_run_event event) override {
    if (record.id.empty()) {
      throw std::invalid_argument("agent run store requires a run id");
    }
    if (event.type.empty()) {
      throw std::invalid_argument("agent run store requires an event type");
    }
    std::scoped_lock lock(mutex_);
    if (records_.contains(record.id)) {
      return {
        .status = run_store_write_status::already_exists,
        .revision = records_.at(record.id).revision,
      };
    }
    record.revision = 1;
    event.run_id = record.id;
    event.sequence = record.revision;
    event.status = record.status;
    records_[record.id] = record;
    events_[record.id].push_back(std::move(event));
    return { .revision = record.revision };
  }

  std::optional<agent_run_record> load(const std::string& run_id) const override {
    std::scoped_lock lock(mutex_);
    const auto found = records_.find(run_id);
    return found == records_.end() ? std::nullopt : std::optional<agent_run_record>(found->second);
  }

  run_store_write_result update(
    std::uint64_t expected_revision, agent_run_record record, agent_run_event event) override {
    if (record.id.empty()) {
      throw std::invalid_argument("agent run store requires a run id");
    }
    if (event.type.empty()) {
      throw std::invalid_argument("agent run store requires an event type");
    }
    std::scoped_lock lock(mutex_);
    const auto found = records_.find(record.id);
    if (found == records_.end()) {
      return { .status = run_store_write_status::not_found };
    }
    if (found->second.revision != expected_revision) {
      return {
        .status = run_store_write_status::conflict,
        .revision = found->second.revision,
      };
    }
    record.revision = expected_revision + 1;
    event.run_id = record.id;
    event.sequence = record.revision;
    event.status = record.status;
    found->second = record;
    events_[record.id].push_back(std::move(event));
    return { .revision = record.revision };
  }

  std::vector<agent_run_event> list_events(
    const std::string& run_id, std::uint64_t after_sequence = 0) const override {
    std::scoped_lock lock(mutex_);
    std::vector<agent_run_event> output;
    const auto found = events_.find(run_id);
    if (found == events_.end()) {
      return output;
    }
    for (const auto& event : found->second) {
      if (event.sequence > after_sequence) {
        output.push_back(event);
      }
    }
    return output;
  }

private:
  mutable std::mutex mutex_;
  std::map<std::string, agent_run_record> records_;
  std::map<std::string, std::vector<agent_run_event>> events_;
};

} // namespace wuwe::agent::runtime

#endif // WUWE_AGENT_RUNTIME_RUN_STORE_HPP
