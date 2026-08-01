#include <wuwe/agent/skills/skill_version.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <stdexcept>

namespace wuwe::agent::skills {
namespace {

bool ascii_alphanumeric_or_hyphen(char value) {
  const auto byte = static_cast<unsigned char>(value);
  return std::isalnum(byte) != 0 || value == '-';
}

bool numeric_identifier(std::string_view value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), [](char item) {
    return item >= '0' && item <= '9';
  });
}

std::uint64_t parse_core_number(std::string_view value) {
  if (value.empty() || (value.size() > 1 && value.front() == '0')) {
    throw std::invalid_argument("semantic version has an invalid numeric identifier");
  }
  std::uint64_t output = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), output);
  if (result.ec != std::errc {} || result.ptr != value.data() + value.size()) {
    throw std::invalid_argument("semantic version numeric identifier is out of range");
  }
  return output;
}

std::vector<std::string> parse_identifiers(std::string_view value, bool prerelease) {
  std::vector<std::string> output;
  std::size_t offset = 0;
  while (offset <= value.size()) {
    const auto end = value.find('.', offset);
    const auto item =
      value.substr(offset, end == std::string_view::npos ? value.size() - offset : end - offset);
    if (item.empty() || !std::all_of(item.begin(), item.end(), ascii_alphanumeric_or_hyphen) ||
        (prerelease && numeric_identifier(item) && item.size() > 1 && item.front() == '0')) {
      throw std::invalid_argument("semantic version has an invalid identifier");
    }
    output.emplace_back(item);
    if (end == std::string_view::npos) {
      break;
    }
    offset = end + 1;
  }
  return output;
}

std::strong_ordering compare_prerelease_identifier(
  std::string_view lhs, std::string_view rhs) noexcept {
  const bool lhs_numeric = numeric_identifier(lhs);
  const bool rhs_numeric = numeric_identifier(rhs);
  if (lhs_numeric && rhs_numeric) {
    if (lhs.size() != rhs.size()) {
      return lhs.size() < rhs.size() ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    if (lhs == rhs) {
      return std::strong_ordering::equal;
    }
    return lhs < rhs ? std::strong_ordering::less : std::strong_ordering::greater;
  }
  if (lhs_numeric != rhs_numeric) {
    return lhs_numeric ? std::strong_ordering::less : std::strong_ordering::greater;
  }
  if (lhs == rhs) {
    return std::strong_ordering::equal;
  }
  return lhs < rhs ? std::strong_ordering::less : std::strong_ordering::greater;
}

std::string_view trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return value;
}

semantic_version upper_for_caret(const semantic_version& version) {
  if (version.major != 0) {
    if (version.major == std::numeric_limits<std::uint64_t>::max()) {
      throw std::invalid_argument("caret version range upper bound overflows");
    }
    return { .major = version.major + 1 };
  }
  if (version.minor != 0) {
    if (version.minor == std::numeric_limits<std::uint64_t>::max()) {
      throw std::invalid_argument("caret version range upper bound overflows");
    }
    return { .minor = version.minor + 1 };
  }
  if (version.patch == std::numeric_limits<std::uint64_t>::max()) {
    throw std::invalid_argument("caret version range upper bound overflows");
  }
  return { .patch = version.patch + 1 };
}

semantic_version upper_for_tilde(const semantic_version& version) {
  if (version.minor == std::numeric_limits<std::uint64_t>::max()) {
    throw std::invalid_argument("tilde version range upper bound overflows");
  }
  return { .major = version.major, .minor = version.minor + 1 };
}

} // namespace

semantic_version semantic_version::parse(std::string_view value) {
  if (value.empty() || value != trim(value)) {
    throw std::invalid_argument("semantic version must not be empty or contain surrounding space");
  }

  semantic_version output;
  const auto plus = value.find('+');
  if (plus != std::string_view::npos) {
    if (value.find('+', plus + 1) != std::string_view::npos) {
      throw std::invalid_argument("semantic version contains multiple build separators");
    }
    output.build = parse_identifiers(value.substr(plus + 1), false);
    value = value.substr(0, plus);
  }
  const auto dash = value.find('-');
  if (dash != std::string_view::npos) {
    output.prerelease = parse_identifiers(value.substr(dash + 1), true);
    value = value.substr(0, dash);
  }

  const auto first_dot = value.find('.');
  const auto second_dot =
    first_dot == std::string_view::npos ? std::string_view::npos : value.find('.', first_dot + 1);
  if (first_dot == std::string_view::npos || second_dot == std::string_view::npos ||
      value.find('.', second_dot + 1) != std::string_view::npos) {
    throw std::invalid_argument("semantic version must contain major.minor.patch");
  }
  output.major = parse_core_number(value.substr(0, first_dot));
  output.minor = parse_core_number(value.substr(first_dot + 1, second_dot - first_dot - 1));
  output.patch = parse_core_number(value.substr(second_dot + 1));
  return output;
}

bool semantic_version::try_parse(std::string_view value, semantic_version& output) noexcept {
  try {
    auto parsed = parse(value);
    output = std::move(parsed);
    return true;
  }
  catch (...) {
    return false;
  }
}

std::string semantic_version::string() const {
  auto output = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
  const auto append = [&output](char delimiter, const std::vector<std::string>& identifiers) {
    if (identifiers.empty()) {
      return;
    }
    output.push_back(delimiter);
    for (std::size_t index = 0; index < identifiers.size(); ++index) {
      if (index != 0) {
        output.push_back('.');
      }
      output += identifiers[index];
    }
  };
  append('-', prerelease);
  append('+', build);
  return output;
}

std::strong_ordering compare_precedence(
  const semantic_version& lhs, const semantic_version& rhs) noexcept {
  if (const auto value = lhs.major <=> rhs.major; value != 0) {
    return value;
  }
  if (const auto value = lhs.minor <=> rhs.minor; value != 0) {
    return value;
  }
  if (const auto value = lhs.patch <=> rhs.patch; value != 0) {
    return value;
  }
  if (lhs.prerelease.empty() != rhs.prerelease.empty()) {
    return lhs.prerelease.empty() ? std::strong_ordering::greater : std::strong_ordering::less;
  }
  for (std::size_t index = 0; index < std::min(lhs.prerelease.size(), rhs.prerelease.size());
       ++index) {
    const auto value = compare_prerelease_identifier(lhs.prerelease[index], rhs.prerelease[index]);
    if (value != 0) {
      return value;
    }
  }
  return lhs.prerelease.size() <=> rhs.prerelease.size();
}

std::strong_ordering operator<=>(
  const semantic_version& lhs, const semantic_version& rhs) noexcept {
  if (const auto precedence = compare_precedence(lhs, rhs); precedence != 0) {
    return precedence;
  }
  return lhs.build <=> rhs.build;
}

bool operator==(const semantic_version& lhs, const semantic_version& rhs) noexcept {
  return lhs.major == rhs.major && lhs.minor == rhs.minor && lhs.patch == rhs.patch &&
         lhs.prerelease == rhs.prerelease && lhs.build == rhs.build;
}

version_requirement version_requirement::any() {
  return {};
}

version_requirement version_requirement::exact(semantic_version version) {
  version_requirement output;
  output.expression_ = version.string();
  output.allows_prerelease_ = !version.prerelease.empty();
  output.comparators_.push_back({ version_comparison::equal, std::move(version) });
  return output;
}

version_requirement version_requirement::parse(std::string_view expression) {
  expression = trim(expression);
  if (expression.empty()) {
    throw std::invalid_argument("version requirement must not be empty");
  }
  if (expression.size() > 4096) {
    throw std::invalid_argument("version requirement exceeds the supported length");
  }

  version_requirement output;
  output.expression_ = std::string(expression);
  if (expression == "*" || expression == "any") {
    return output;
  }

  for (std::size_t index = 0; index < expression.size(); ++index) {
    if (expression[index] != ',') {
      continue;
    }
    const auto before = expression.find_last_not_of(" \t\r\n", index == 0 ? 0 : index - 1);
    const auto after = expression.find_first_not_of(" \t\r\n", index + 1);
    if (index == 0 || before == std::string_view::npos || expression[before] == ',' ||
        after == std::string_view::npos || expression[after] == ',') {
      throw std::invalid_argument("version requirement contains an empty comparator");
    }
  }

  bool allows_prerelease = false;
  std::size_t offset = 0;
  while (offset < expression.size()) {
    while (offset < expression.size() &&
           (std::isspace(static_cast<unsigned char>(expression[offset])) != 0 ||
             expression[offset] == ',')) {
      ++offset;
    }
    if (offset == expression.size()) {
      break;
    }
    const auto end = expression.find_first_of(" ,\t\r\n", offset);
    auto token = expression.substr(
      offset, end == std::string_view::npos ? expression.size() - offset : end - offset);
    version_comparison comparison = version_comparison::equal;
    bool caret = false;
    bool tilde = false;
    if (token.starts_with(">=")) {
      comparison = version_comparison::greater_equal;
      token.remove_prefix(2);
    }
    else if (token.starts_with("<=")) {
      comparison = version_comparison::less_equal;
      token.remove_prefix(2);
    }
    else if (token.starts_with('>')) {
      comparison = version_comparison::greater;
      token.remove_prefix(1);
    }
    else if (token.starts_with('<')) {
      comparison = version_comparison::less;
      token.remove_prefix(1);
    }
    else if (token.starts_with('=')) {
      token.remove_prefix(1);
    }
    else if (token.starts_with('^')) {
      caret = true;
      token.remove_prefix(1);
    }
    else if (token.starts_with('~')) {
      tilde = true;
      token.remove_prefix(1);
    }
    if (token.empty()) {
      throw std::invalid_argument("version requirement contains a missing version");
    }
    auto version = semantic_version::parse(token);
    allows_prerelease = allows_prerelease || !version.prerelease.empty();
    if (caret || tilde) {
      const auto upper = caret ? upper_for_caret(version) : upper_for_tilde(version);
      output.comparators_.push_back({ version_comparison::greater_equal, version });
      output.comparators_.push_back({ version_comparison::less, upper });
    }
    else {
      output.comparators_.push_back({ comparison, std::move(version) });
    }
    offset = end == std::string_view::npos ? expression.size() : end;
  }
  if (output.comparators_.empty()) {
    throw std::invalid_argument("version requirement contains no comparators");
  }
  output.allows_prerelease_ = allows_prerelease;
  return output;
}

bool version_requirement::try_parse(
  std::string_view expression, version_requirement& output) noexcept {
  try {
    auto parsed = parse(expression);
    output = std::move(parsed);
    return true;
  }
  catch (...) {
    return false;
  }
}

bool version_requirement::matches(const semantic_version& version) const noexcept {
  if (!version.stable() && !allows_prerelease_) {
    return false;
  }
  return std::all_of(comparators_.begin(), comparators_.end(), [&](const auto& comparator) {
    switch (comparator.comparison) {
      case version_comparison::equal:
        return version == comparator.version;
      case version_comparison::less:
        return compare_precedence(version, comparator.version) < 0;
      case version_comparison::less_equal:
        return compare_precedence(version, comparator.version) <= 0;
      case version_comparison::greater:
        return compare_precedence(version, comparator.version) > 0;
      case version_comparison::greater_equal:
        return compare_precedence(version, comparator.version) >= 0;
    }
    return false;
  });
}

} // namespace wuwe::agent::skills
