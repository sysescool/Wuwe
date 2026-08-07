#ifndef WUWE_AGENT_LEARNING_LEARNING_STORE_HPP
#define WUWE_AGENT_LEARNING_LEARNING_STORE_HPP

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <wuwe/agent/learning/learning_core.hpp>

namespace wuwe::agent::learning {

class learning_store {
public:
  virtual ~learning_store() = default;
  virtual void save(const learning_record& value) = 0;
  [[nodiscard]] virtual std::optional<learning_record> load(const std::string& id) const = 0;
  [[nodiscard]] virtual std::vector<learning_record> list() const = 0;
  virtual bool erase(const std::string& id) = 0;
};

class in_memory_learning_store final : public learning_store {
public:
  void save(const learning_record& value) override {
    std::scoped_lock lock(mutex_);
    records_[value.id] = value;
  }

  [[nodiscard]] std::optional<learning_record> load(const std::string& id) const override {
    std::scoped_lock lock(mutex_);
    const auto found = records_.find(id);
    return found == records_.end() ? std::nullopt : std::optional(found->second);
  }

  [[nodiscard]] std::vector<learning_record> list() const override {
    std::scoped_lock lock(mutex_);
    std::vector<learning_record> output;
    output.reserve(records_.size());
    for (const auto& [_, value] : records_)
      output.push_back(value);
    return output;
  }

  bool erase(const std::string& id) override {
    std::scoped_lock lock(mutex_);
    return records_.erase(id) != 0;
  }

private:
  mutable std::mutex mutex_;
  std::map<std::string, learning_record> records_;
};

} // namespace wuwe::agent::learning

#endif // WUWE_AGENT_LEARNING_LEARNING_STORE_HPP
