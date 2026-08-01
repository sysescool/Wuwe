#include <wuwe/agent/skills/directory_skill_loader.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <wuwe/common/sha256.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wuwe::agent::skills {
namespace {

constexpr std::string_view manifest_name = "skill.json";
constexpr std::size_t maximum_portable_path_bytes = 1024;
constexpr std::size_t maximum_portable_component_bytes = 255;
constexpr std::size_t maximum_portable_path_components = 32;

class load_failure final : public std::runtime_error {
public:
  load_failure(skill_load_error_code code, std::string message)
      : std::runtime_error(std::move(message)), code(code) {
  }

  skill_load_error_code code;
};

[[noreturn]] void fail(skill_load_error_code code, std::string message) {
  throw load_failure(code, std::move(message));
}

std::string path_to_utf8(const std::filesystem::path& path) {
  const auto value = path.generic_u8string();
  return { reinterpret_cast<const char*>(value.data()), value.size() };
}

std::string native_path_to_utf8(const std::filesystem::path& path) {
  const auto value = path.u8string();
  return { reinterpret_cast<const char*>(value.data()), value.size() };
}

std::filesystem::path path_from_utf8(std::string_view value) {
  std::u8string converted;
  converted.reserve(value.size());
  for (const auto ch : value) {
    converted.push_back(static_cast<char8_t>(static_cast<unsigned char>(ch)));
  }
  return std::filesystem::path(converted);
}

std::string portable_case_key(std::string_view value) {
#if defined(_WIN32)
  const auto path = path_from_utf8(value);
  auto wide = path.generic_wstring();
  if (!wide.empty()) {
    const auto length = LCMapStringEx(LOCALE_NAME_INVARIANT,
      LCMAP_LOWERCASE,
      wide.data(),
      static_cast<int>(wide.size()),
      nullptr,
      0,
      nullptr,
      nullptr,
      0);
    if (length > 0) {
      std::wstring folded(static_cast<std::size_t>(length), L'\0');
      if (LCMapStringEx(LOCALE_NAME_INVARIANT,
            LCMAP_LOWERCASE,
            wide.data(),
            static_cast<int>(wide.size()),
            folded.data(),
            length,
            nullptr,
            nullptr,
            0) > 0) {
        return path_to_utf8(std::filesystem::path(folded));
      }
    }
  }
#endif
  std::string output(value);
  std::transform(output.begin(), output.end(), output.begin(), [](unsigned char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : static_cast<char>(ch);
  });
  return output;
}

bool windows_reserved_component(std::string_view component) {
  auto end = component.find('.');
  auto stem = portable_case_key(component.substr(0, end));
  while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) {
    stem.pop_back();
  }
  if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") {
    return true;
  }
  if (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' &&
      stem[3] <= '9') {
    return true;
  }
  return false;
}

void validate_relative_path_text(std::string_view value, std::string_view purpose) {
  if (value.empty()) {
    fail(skill_load_error_code::invalid_path, std::string(purpose) + " path is empty");
  }
  if (value.size() > maximum_portable_path_bytes) {
    fail(skill_load_error_code::invalid_path,
      std::string(purpose) + " path exceeds the portable length limit");
  }
  if (value.front() == '/' || value.find('\\') != std::string_view::npos ||
      value.find('\0') != std::string_view::npos) {
    fail(skill_load_error_code::invalid_path,
      std::string(purpose) + " path must use a relative portable path");
  }

  std::size_t begin = 0;
  std::size_t component_count = 0;
  while (begin <= value.size()) {
    const auto end = value.find('/', begin);
    const auto component =
      value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin);
    if (component.empty() || component == "." || component == "..") {
      fail(skill_load_error_code::invalid_path,
        std::string(purpose) + " path contains an invalid component");
    }
    ++component_count;
    if (component_count > maximum_portable_path_components ||
        component.size() > maximum_portable_component_bytes) {
      fail(skill_load_error_code::invalid_path,
        std::string(purpose) + " path exceeds the portable depth or component limit");
    }
    if (component.back() == ' ' || component.back() == '.' ||
        windows_reserved_component(component)) {
      fail(skill_load_error_code::invalid_path,
        std::string(purpose) + " path is not portable across supported filesystems");
    }
    for (const auto raw_ch : component) {
      const auto ch = static_cast<unsigned char>(raw_ch);
      if (ch < 0x20U || ch == 0x7fU || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
          ch == '|' || ch == '?' || ch == '*') {
        fail(skill_load_error_code::invalid_path,
          std::string(purpose) + " path contains a forbidden character");
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }

  const auto parsed = path_from_utf8(value);
  if (parsed.is_absolute() || parsed.has_root_name() || parsed.has_root_directory()) {
    fail(skill_load_error_code::invalid_path, std::string(purpose) + " path must be relative");
  }
}

bool path_component_equal(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
  return portable_case_key(path_to_utf8(lhs)) == portable_case_key(path_to_utf8(rhs));
}

bool path_within(const std::filesystem::path& path, const std::filesystem::path& root) {
  auto path_it = path.begin();
  for (auto root_it = root.begin(); root_it != root.end(); ++root_it, ++path_it) {
    if (path_it == path.end() || !path_component_equal(*path_it, *root_it)) {
      return false;
    }
  }
  return true;
}

bool is_reparse_point(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error) {
    fail(skill_load_error_code::filesystem_error, "failed to inspect path: " + path_to_utf8(path));
  }
  if (std::filesystem::is_symlink(status)) {
    return true;
  }
#if defined(_WIN32)
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    fail(skill_load_error_code::filesystem_error,
      "failed to inspect path attributes: " + path_to_utf8(path));
  }
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  return false;
#endif
}

void reject_reparse_components(
  const std::filesystem::path& root, const std::filesystem::path& target) {
  if (!path_within(target, root)) {
    fail(skill_load_error_code::path_outside_root, "skill path escapes the configured root");
  }
  auto current = root;
  if (is_reparse_point(current)) {
    fail(skill_load_error_code::reparse_point, "configured skill root is a reparse point");
  }
  auto target_it = target.begin();
  for (auto root_it = root.begin(); root_it != root.end(); ++root_it) {
    ++target_it;
  }
  for (; target_it != target.end(); ++target_it) {
    current /= *target_it;
    if (is_reparse_point(current)) {
      fail(skill_load_error_code::reparse_point,
        "skill package contains a reparse point: " + path_to_utf8(current));
    }
  }
}

void reject_hard_link(const std::filesystem::path& path, bool enabled) {
  if (!enabled) {
    return;
  }
  std::error_code error;
  const auto count = std::filesystem::hard_link_count(path, error);
  if (error) {
    fail(skill_load_error_code::filesystem_error,
      "failed to inspect resource link count: " + path_to_utf8(path));
  }
  if (count > 1) {
    fail(skill_load_error_code::hard_link,
      "skill package file has multiple hard links: " + path_to_utf8(path));
  }
}

struct loaded_file {
  std::string content;
  std::string digest;
};

loaded_file read_file(const std::filesystem::path& path, std::size_t maximum_size,
  skill_load_error_code too_large_code, bool reject_hard_links) {
  if (is_reparse_point(path)) {
    fail(skill_load_error_code::reparse_point,
      "skill package file is a reparse point: " + path_to_utf8(path));
  }
  std::error_code error;
  const auto status = std::filesystem::status(path, error);
  if (error || !std::filesystem::exists(status)) {
    fail(skill_load_error_code::resource_missing,
      "skill package file does not exist: " + path_to_utf8(path));
  }
  if (!std::filesystem::is_regular_file(status)) {
    fail(skill_load_error_code::resource_not_regular,
      "skill package resource is not a regular file: " + path_to_utf8(path));
  }
  reject_hard_link(path, reject_hard_links);

  const auto raw_size = std::filesystem::file_size(path, error);
  if (error) {
    fail(skill_load_error_code::filesystem_error,
      "failed to determine file size: " + path_to_utf8(path));
  }
  if (raw_size > maximum_size || raw_size > std::numeric_limits<std::size_t>::max()) {
    fail(too_large_code, "skill package file exceeds its configured size limit");
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    fail(skill_load_error_code::filesystem_error,
      "failed to open skill package file: " + path_to_utf8(path));
  }
  loaded_file output;
  output.content.reserve(static_cast<std::size_t>(raw_size));
  common::sha256 hash;
  std::array<char, 64 * 1024> buffer {};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      const auto size = static_cast<std::size_t>(count);
      if (output.content.size() > maximum_size - size) {
        fail(too_large_code, "skill package file exceeds its configured size limit");
      }
      output.content.append(buffer.data(), size);
      hash.update(std::string_view(buffer.data(), size));
    }
  }
  if (!input.eof()) {
    fail(skill_load_error_code::filesystem_error,
      "failed while reading skill package file: " + path_to_utf8(path));
  }
  if (output.content.size() != raw_size) {
    fail(skill_load_error_code::filesystem_error, "skill package file changed while being read");
  }
  if (is_reparse_point(path)) {
    fail(skill_load_error_code::reparse_point, "skill package file changed to a reparse point");
  }
  const auto final_size = std::filesystem::file_size(path, error);
  if (error || final_size != raw_size) {
    fail(skill_load_error_code::filesystem_error, "skill package file changed while being read");
  }
  output.digest = hash.hex_digest();
  return output;
}

void hash_u64(common::sha256& hash, std::uint64_t value) {
  std::array<std::uint8_t, 8> bytes {};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[bytes.size() - 1 - i] = static_cast<std::uint8_t>(value >> (i * 8U));
  }
  hash.update(std::span<const std::uint8_t>(bytes));
}

void hash_field(common::sha256& hash, std::string_view value) {
  hash_u64(hash, static_cast<std::uint64_t>(value.size()));
  hash.update(value);
}

std::string package_digest(
  const loaded_file& manifest, const std::map<std::string, skill_resource>& resources) {
  common::sha256 hash;
  constexpr char digest_domain[] = "wuwe.skill.package.v1\0";
  hash.update(std::string_view(digest_domain, sizeof(digest_domain) - 1));
  hash_field(hash, manifest_name);
  hash_field(hash, manifest.content);

  std::vector<const skill_resource*> ordered;
  ordered.reserve(resources.size());
  for (const auto& [_, resource] : resources) {
    ordered.push_back(&resource);
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto* lhs, const auto* rhs) {
    return lhs->descriptor.path < rhs->descriptor.path;
  });
  hash_u64(hash, static_cast<std::uint64_t>(ordered.size()));
  for (const auto* resource : ordered) {
    hash_field(hash, resource->descriptor.path);
    hash_field(hash, to_string(resource->descriptor.kind));
    hash_u64(hash, static_cast<std::uint64_t>(resource->descriptor.size));
    hash_field(hash, resource->sha256);
  }
  return hash.hex_digest();
}

std::string file_uri(const std::filesystem::path& path) {
  const auto generic = path_to_utf8(path);
  constexpr char hex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(generic.size());
  for (const auto raw_ch : generic) {
    const auto ch = static_cast<unsigned char>(raw_ch);
    const auto unreserved = std::isalnum(ch) != 0 || ch == '-' || ch == '.' || ch == '_' ||
                            ch == '~' || ch == '/' || ch == ':';
    if (unreserved) {
      encoded.push_back(static_cast<char>(ch));
    }
    else {
      encoded.push_back('%');
      encoded.push_back(hex[ch >> 4U]);
      encoded.push_back(hex[ch & 0x0fU]);
    }
  }
#if defined(_WIN32)
  return "file:///" + encoded;
#else
  return "file://" + encoded;
#endif
}

skill_load_result failure_result(
  skill_load_error_code code, std::string message, std::string skill_id = {}) {
  skill_load_result result;
  result.error = code;
  result.message = std::move(message);
  result.diagnostics.push_back({
    .severity = skill_diagnostic_severity::error,
    .code = to_string(code),
    .message = result.message,
    .skill_id = std::move(skill_id),
  });
  return result;
}

void validate_options(const directory_skill_loader_options& options) {
  if (options.root.empty() || !options.root.is_absolute()) {
    fail(skill_load_error_code::invalid_options, "skill loader root must be an absolute path");
  }
  if (options.max_manifest_bytes == 0 || options.max_resource_bytes == 0 ||
      options.max_package_bytes == 0 || options.max_resources == 0) {
    fail(skill_load_error_code::invalid_options, "skill loader limits must be positive");
  }
  if (options.max_manifest_bytes > options.max_package_bytes ||
      options.max_resource_bytes > options.max_package_bytes) {
    fail(skill_load_error_code::invalid_options,
      "individual skill file limits cannot exceed the package size limit");
  }
}

} // namespace

const char* to_string(skill_load_error_code value) noexcept {
  switch (value) {
    case skill_load_error_code::none:
      return "none";
    case skill_load_error_code::invalid_options:
      return "invalid_options";
    case skill_load_error_code::invalid_path:
      return "invalid_path";
    case skill_load_error_code::root_not_found:
      return "root_not_found";
    case skill_load_error_code::root_not_directory:
      return "root_not_directory";
    case skill_load_error_code::path_outside_root:
      return "path_outside_root";
    case skill_load_error_code::filesystem_error:
      return "filesystem_error";
    case skill_load_error_code::reparse_point:
      return "reparse_point";
    case skill_load_error_code::hard_link:
      return "hard_link";
    case skill_load_error_code::case_collision:
      return "case_collision";
    case skill_load_error_code::manifest_missing:
      return "manifest_missing";
    case skill_load_error_code::manifest_too_large:
      return "manifest_too_large";
    case skill_load_error_code::manifest_invalid:
      return "manifest_invalid";
    case skill_load_error_code::resource_count_exceeded:
      return "resource_count_exceeded";
    case skill_load_error_code::duplicate_resource_path:
      return "duplicate_resource_path";
    case skill_load_error_code::undeclared_resource:
      return "undeclared_resource";
    case skill_load_error_code::resource_missing:
      return "resource_missing";
    case skill_load_error_code::resource_not_regular:
      return "resource_not_regular";
    case skill_load_error_code::resource_too_large:
      return "resource_too_large";
    case skill_load_error_code::package_too_large:
      return "package_too_large";
    case skill_load_error_code::resource_size_mismatch:
      return "resource_size_mismatch";
    case skill_load_error_code::resource_digest_mismatch:
      return "resource_digest_mismatch";
    case skill_load_error_code::package_invalid:
      return "package_invalid";
  }
  return "filesystem_error";
}

directory_skill_loader::directory_skill_loader(directory_skill_loader_options options)
    : options_(std::move(options)) {
}

skill_load_result directory_skill_loader::load(const std::filesystem::path& relative_path,
  observability::event_sink* event_sink,
  const skill_observability_context& context) const noexcept {
  publish_skill_event(
    event_sink, context, "skill.load.started", { { "origin", "local_directory" } });

  std::string skill_id;
  try {
    validate_options(options_);

    const auto request_text = native_path_to_utf8(relative_path);
    validate_relative_path_text(request_text, "skill package");

    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(options_.root, error);
    if (error || !std::filesystem::exists(root_status)) {
      fail(skill_load_error_code::root_not_found, "configured skill root does not exist");
    }
    if (std::filesystem::is_symlink(root_status) || is_reparse_point(options_.root)) {
      fail(skill_load_error_code::reparse_point, "configured skill root is a reparse point");
    }
    if (!std::filesystem::is_directory(root_status)) {
      fail(skill_load_error_code::root_not_directory, "configured skill root is not a directory");
    }

    const auto canonical_root = std::filesystem::canonical(options_.root, error);
    if (error) {
      fail(skill_load_error_code::filesystem_error, "failed to resolve configured skill root");
    }
    const auto requested_package = canonical_root / path_from_utf8(request_text);
    const auto package_status = std::filesystem::symlink_status(requested_package, error);
    if (error || !std::filesystem::exists(package_status)) {
      fail(skill_load_error_code::root_not_found, "skill package directory does not exist");
    }
    if (std::filesystem::is_symlink(package_status) || is_reparse_point(requested_package)) {
      fail(skill_load_error_code::reparse_point, "skill package directory is a reparse point");
    }
    if (!std::filesystem::is_directory(package_status)) {
      fail(skill_load_error_code::root_not_directory, "skill package path is not a directory");
    }
    reject_reparse_components(canonical_root, requested_package);
    const auto canonical_package = std::filesystem::canonical(requested_package, error);
    if (error || !path_within(canonical_package, canonical_root)) {
      fail(skill_load_error_code::path_outside_root, "skill package resolves outside its root");
    }

    const auto manifest_path = canonical_package / path_from_utf8(manifest_name);
    const auto manifest_status = std::filesystem::symlink_status(manifest_path, error);
    if (error || !std::filesystem::exists(manifest_status)) {
      fail(skill_load_error_code::manifest_missing, "skill package does not contain skill.json");
    }
    auto raw_manifest = read_file(manifest_path,
      options_.max_manifest_bytes,
      skill_load_error_code::manifest_too_large,
      options_.reject_hard_links);

    skill_manifest manifest;
    try {
      manifest = parse_skill_manifest(
        raw_manifest.content, { .max_manifest_bytes = options_.max_manifest_bytes });
    }
    catch (const std::exception& exception) {
      fail(skill_load_error_code::manifest_invalid,
        "invalid skill manifest: " + std::string(exception.what()));
    }
    skill_id = manifest.descriptor.id;
    if (manifest.resources.size() > options_.max_resources) {
      fail(skill_load_error_code::resource_count_exceeded,
        "skill manifest exceeds the configured resource count limit");
    }

    std::map<std::string, const skill_resource_descriptor*> declared_paths;
    for (const auto& descriptor : manifest.resources) {
      validate_relative_path_text(descriptor.path, "skill resource");
      const auto declared_digest_is_lowercase =
        descriptor.sha256.size() == 64 &&
        std::all_of(descriptor.sha256.begin(), descriptor.sha256.end(), [](unsigned char ch) {
          return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
      if (!declared_digest_is_lowercase) {
        fail(skill_load_error_code::manifest_invalid,
          "skill resources must declare a lowercase SHA-256 digest");
      }
      const auto key = portable_case_key(descriptor.path);
      if (key == portable_case_key(manifest_name)) {
        fail(skill_load_error_code::duplicate_resource_path,
          "skill resource path collides with skill.json");
      }
      if (!declared_paths.emplace(key, &descriptor).second) {
        fail(skill_load_error_code::duplicate_resource_path,
          "skill resource paths must be unique without regard to case");
      }
    }

    std::set<std::string> discovered_paths;
    for (std::filesystem::recursive_directory_iterator
           iterator(canonical_package, std::filesystem::directory_options::none, error),
         end;
         iterator != end;
         iterator.increment(error)) {
      if (error) {
        fail(skill_load_error_code::filesystem_error, "failed while enumerating the skill package");
      }
      const auto& entry = *iterator;
      if (is_reparse_point(entry.path())) {
        iterator.disable_recursion_pending();
        fail(skill_load_error_code::reparse_point,
          "skill package contains a reparse point: " + path_to_utf8(entry.path()));
      }
      const auto relative = std::filesystem::relative(entry.path(), canonical_package, error);
      if (error) {
        fail(skill_load_error_code::filesystem_error,
          "failed to determine a skill package relative path");
      }
      const auto relative_text = path_to_utf8(relative);
      validate_relative_path_text(relative_text, "skill package entry");
      const auto key = portable_case_key(relative_text);
      if (!discovered_paths.emplace(key).second) {
        fail(skill_load_error_code::case_collision,
          "skill package contains paths that differ only by case");
      }
      if (entry.is_regular_file(error)) {
        if (error) {
          fail(skill_load_error_code::filesystem_error, "failed to inspect a skill package entry");
        }
        if (key != portable_case_key(manifest_name) && !declared_paths.contains(key)) {
          fail(skill_load_error_code::undeclared_resource,
            "skill package contains an undeclared file: " + relative_text);
        }
      }
      else if (error || !entry.is_directory(error)) {
        fail(skill_load_error_code::resource_not_regular,
          "skill package contains a non-regular filesystem entry");
      }
    }
    if (error) {
      fail(skill_load_error_code::filesystem_error, "failed while enumerating the skill package");
    }

    std::size_t package_size = raw_manifest.content.size();
    std::map<std::string, skill_resource> resources;
    for (const auto& descriptor : manifest.resources) {
      const auto resource_path = canonical_package / path_from_utf8(descriptor.path);
      const auto resource_status = std::filesystem::symlink_status(resource_path, error);
      if (error || !std::filesystem::exists(resource_status)) {
        fail(skill_load_error_code::resource_missing,
          "declared skill resource does not exist: " + descriptor.id);
      }
      reject_reparse_components(canonical_package, resource_path);
      auto loaded = read_file(resource_path,
        options_.max_resource_bytes,
        skill_load_error_code::resource_too_large,
        options_.reject_hard_links);
      if (loaded.content.size() != descriptor.size) {
        fail(skill_load_error_code::resource_size_mismatch,
          "skill resource size does not match its manifest declaration: " + descriptor.id);
      }
      if (descriptor.sha256 != loaded.digest) {
        fail(skill_load_error_code::resource_digest_mismatch,
          "skill resource digest does not match its manifest declaration: " + descriptor.id);
      }
      if (loaded.content.size() > options_.max_package_bytes - package_size) {
        fail(skill_load_error_code::package_too_large,
          "skill package exceeds the configured aggregate size limit");
      }
      package_size += loaded.content.size();
      resources.emplace(descriptor.id,
        skill_resource {
          .descriptor = descriptor,
          .content = std::move(loaded.content),
          .sha256 = std::move(loaded.digest),
        });
    }

    skill_provenance provenance {
      .origin = skill_origin_kind::local,
      .source_uri = file_uri(canonical_package),
      .content_sha256 = package_digest(raw_manifest, resources),
      .trust = options_.trust,
      .metadata = {
        { "manifest_sha256", raw_manifest.digest },
      },
    };
    auto package = std::make_shared<const skill_package>(
      std::move(manifest), std::move(provenance), std::move(resources));

    publish_skill_event(event_sink,
      context,
      "skill.load.succeeded",
      {
        { "skill_id", package->descriptor().id },
        { "skill_version", package->descriptor().version.string() },
        { "resource_count", std::to_string(package->resources().size()) },
        { "package_bytes", std::to_string(package_size) },
        { "package_sha256", package->provenance().content_sha256 },
        { "trust", core::to_string(package->provenance().trust) },
      });
    return { .package = std::move(package) };
  }
  catch (const load_failure& exception) {
    publish_skill_event(event_sink,
      context,
      "skill.load.failed",
      {
        { "error_code", to_string(exception.code) },
        { "skill_id", skill_id },
      });
    return failure_result(exception.code, exception.what(), std::move(skill_id));
  }
  catch (const std::exception& exception) {
    publish_skill_event(event_sink,
      context,
      "skill.load.failed",
      {
        { "error_code", to_string(skill_load_error_code::package_invalid) },
        { "skill_id", skill_id },
      });
    return failure_result(
      skill_load_error_code::package_invalid, exception.what(), std::move(skill_id));
  }
  catch (...) {
    publish_skill_event(event_sink,
      context,
      "skill.load.failed",
      { { "error_code", to_string(skill_load_error_code::package_invalid) } });
    return failure_result(skill_load_error_code::package_invalid,
      "skill package loading failed with an unknown error",
      std::move(skill_id));
  }
}

} // namespace wuwe::agent::skills
