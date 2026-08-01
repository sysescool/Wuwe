#ifndef WUWE_AGENT_SKILLS_SKILL_VERSION_HPP
#define WUWE_AGENT_SKILLS_SKILL_VERSION_HPP

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wuwe::agent::skills {

struct semantic_version {
  std::uint64_t major { 0 };
  std::uint64_t minor { 0 };
  std::uint64_t patch { 0 };
  std::vector<std::string> prerelease;
  std::vector<std::string> build;

  [[nodiscard]] static semantic_version parse(std::string_view value);
  [[nodiscard]] static bool try_parse(std::string_view value, semantic_version& output) noexcept;
  [[nodiscard]] std::string string() const;
  [[nodiscard]] bool stable() const noexcept {
    return prerelease.empty();
  }

  friend bool operator==(const semantic_version& lhs, const semantic_version& rhs) noexcept;
  friend std::strong_ordering operator<=>(
    const semantic_version& lhs, const semantic_version& rhs) noexcept;
};

enum class version_comparison {
  equal,
  less,
  less_equal,
  greater,
  greater_equal,
};

struct version_comparator {
  version_comparison comparison { version_comparison::equal };
  semantic_version version;
};

class version_requirement {
public:
  version_requirement() = default;

  [[nodiscard]] static version_requirement any();
  [[nodiscard]] static version_requirement exact(semantic_version version);
  [[nodiscard]] static version_requirement parse(std::string_view expression);
  [[nodiscard]] static bool try_parse(
    std::string_view expression, version_requirement& output) noexcept;

  [[nodiscard]] bool matches(const semantic_version& version) const noexcept;
  [[nodiscard]] bool allows_prerelease() const noexcept {
    return allows_prerelease_;
  }
  [[nodiscard]] const std::vector<version_comparator>& comparators() const noexcept {
    return comparators_;
  }
  [[nodiscard]] const std::string& expression() const noexcept {
    return expression_;
  }

private:
  std::string expression_ { "*" };
  std::vector<version_comparator> comparators_;
  bool allows_prerelease_ { false };
};

} // namespace wuwe::agent::skills

#endif // WUWE_AGENT_SKILLS_SKILL_VERSION_HPP
