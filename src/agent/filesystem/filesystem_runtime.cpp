#include <wuwe/agent/filesystem/filesystem_runtime.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <wuwe/agent/capability/capability.hpp>

namespace wuwe::agent::filesystem {
namespace {

bool path_within(
  const std::filesystem::path& candidate,
  const std::filesystem::path& root) {
  auto candidate_it = candidate.begin();
  auto root_it = root.begin();
  for (; root_it != root.end(); ++root_it, ++candidate_it) {
    if (candidate_it == candidate.end()) return false;
#ifdef _WIN32
    auto left = candidate_it->wstring();
    auto right = root_it->wstring();
    std::transform(left.begin(), left.end(), left.begin(), ::towlower);
    std::transform(right.begin(), right.end(), right.begin(), ::towlower);
    if (left != right) return false;
#else
    if (*candidate_it != *root_it) return false;
#endif
  }
  return true;
}

audit::audit_event_outcome outcome_for(filesystem_status status) {
  switch (status) {
    case filesystem_status::ok: return audit::audit_event_outcome::completed;
    case filesystem_status::cancelled: return audit::audit_event_outcome::cancelled;
    case filesystem_status::permission_denied:
    case filesystem_status::approval_denied:
    case filesystem_status::outside_root:
      return audit::audit_event_outcome::denied;
    default: return audit::audit_event_outcome::failed;
  }
}

void safely_publish(audit::audit_sink* sink, const audit::audit_event& event) noexcept {
  if (!sink) return;
  try { sink->publish(event); }
  catch (...) {}
}

void erase_reserved_metadata(std::map<std::string, std::string>& metadata) {
  static constexpr std::array<std::string_view, 13> reserved {
    "operation_id",
    "operation",
    "partial",
    "parent_directories_created",
    "destination_created",
    "type",
    "size",
    "output_limit_reached",
    "skipped_binary_files",
    "skipped_large_files",
    "skipped_error_files",
    "file_enumeration_truncated",
    "output_bytes",
  };
  for (const auto key : reserved) metadata.erase(std::string(key));
}

template <typename Operation>
filesystem_result safely_invoke_backend(
  const std::filesystem::path& path,
  Operation&& operation) noexcept {
  try {
    return std::forward<Operation>(operation)();
  }
  catch (const std::exception& error) {
    return {
      .status = filesystem_status::io_error,
      .error_message = std::string("filesystem backend failed: ") + error.what(),
      .path = path,
    };
  }
  catch (...) {
    return {
      .status = filesystem_status::io_error,
      .error_message = "filesystem backend failed with an unknown exception",
      .path = path,
    };
  }
}

} // namespace

struct filesystem_runtime::operation_context {
  std::string id;
  std::string operation;
  bool write { false };
  bool approval_required { false };
  std::vector<std::filesystem::path> raw_resources;
  std::vector<std::filesystem::path> resolved_resources;
  std::map<std::string, std::string> metadata;
  filesystem_result failure;
  std::chrono::steady_clock::time_point started { std::chrono::steady_clock::now() };
};

filesystem_runtime::filesystem_runtime(
  std::unique_ptr<filesystem_backend> backend,
  filesystem_policy policy,
  audit::audit_sink* audit,
  approval::approval_service* approvals)
    : backend_(std::move(backend)), policy_(std::move(policy)),
      audit_(audit), approvals_(approvals) {
  if (!backend_) throw std::invalid_argument("filesystem_runtime requires a backend");
  if (policy_.root.empty()) throw std::invalid_argument("filesystem_policy.root is required");
  std::error_code error;
  policy_.root = std::filesystem::weakly_canonical(policy_.root, error);
  if (error || !std::filesystem::is_directory(policy_.root, error)) {
    throw std::invalid_argument("filesystem_policy.root must be an existing directory");
  }
  if (policy_.max_read_bytes == 0 || policy_.max_write_bytes == 0 ||
      policy_.max_search_file_bytes == 0 ||
      policy_.max_search_total_bytes == 0 || policy_.max_copy_bytes == 0 ||
      policy_.max_search_output_bytes == 0 ||
      policy_.max_pattern_bytes == 0 || policy_.max_directory_entries == 0 ||
      policy_.max_search_results == 0 || policy_.max_search_depth == 0) {
    throw std::invalid_argument("filesystem policy limits must be greater than zero");
  }
}

std::unique_ptr<filesystem_runtime::operation_context>
filesystem_runtime::begin_operation(
  std::string operation,
  std::vector<std::filesystem::path> resources,
  bool write,
  bool approval_required,
  std::map<std::string, std::string> metadata) {
  auto context = std::make_unique<operation_context>();
  context->id = "filesystem-" + std::to_string(next_operation_id_.fetch_add(1));
  context->operation = std::move(operation);
  context->write = write;
  context->approval_required = approval_required;
  context->raw_resources = std::move(resources);
  context->metadata = std::move(metadata);
  erase_reserved_metadata(context->metadata);
  context->metadata["operation_id"] = context->id;
  context->metadata["operation"] = context->operation;
  return context;
}

std::optional<std::filesystem::path> filesystem_runtime::resolve_path(
  const std::filesystem::path& path,
  operation_context& context) const {
  if (path.empty()) {
    context.failure = {
      .status = filesystem_status::invalid_path,
      .error_message = "path must not be empty",
    };
    return std::nullopt;
  }
  if (path.is_absolute() && !policy_.allow_absolute_paths) {
    context.failure = {
      .status = filesystem_status::permission_denied,
      .error_message = "absolute paths are not allowed",
      .path = path,
    };
    return std::nullopt;
  }
  const auto candidate = path.is_absolute() ? path : policy_.root / path;
  std::error_code error;
  auto resolved = std::filesystem::weakly_canonical(candidate, error);
  if (error) {
    error.clear();
    resolved = std::filesystem::absolute(candidate, error).lexically_normal();
    if (error) {
      context.failure = {
        .status = filesystem_status::invalid_path,
        .error_message = "failed to normalize path: " + error.message(),
        .path = path,
      };
      return std::nullopt;
    }
  }
  if (!path_within(resolved, policy_.root)) {
    context.failure = {
      .status = filesystem_status::outside_root,
      .error_message = "path resolves outside the configured root",
      .path = path,
    };
    return std::nullopt;
  }
  if (!policy_.follow_symlinks) {
    const auto relative = candidate.lexically_normal().lexically_relative(policy_.root);
    auto current = policy_.root;
    for (const auto& component : relative) {
      current /= component;
      const auto status = std::filesystem::symlink_status(current, error);
      if (error == std::errc::no_such_file_or_directory) {
        error.clear();
        break;
      }
      if (error) {
        context.failure = {
          .status = filesystem_status::invalid_path,
          .error_message = "failed to inspect path for symbolic links: " + error.message(),
          .path = path,
        };
        return std::nullopt;
      }
      if (std::filesystem::is_symlink(status)) {
        context.failure = {
          .status = filesystem_status::permission_denied,
          .error_message = "symbolic links are disabled by policy",
          .path = path,
        };
        return std::nullopt;
      }
    }
  }
  context.resolved_resources.push_back(resolved);
  return resolved;
}

bool filesystem_runtime::authorize(
  operation_context& context,
  std::stop_token stop_token) {
  if (!context.failure.error_message.empty()) return false;
  if (stop_token.stop_requested()) {
    context.failure = {
      .status = filesystem_status::cancelled,
      .error_message = "operation cancelled before authorization",
    };
    return false;
  }
  capability::capability_request capability_request {
    .name = context.write ? capability::names::filesystem_write
                          : capability::names::filesystem_read,
    .risk = context.write ? capability::capability_risk_level::high
                          : capability::capability_risk_level::low,
    .summary = context.operation + " within filesystem root",
    .tool_name = context.operation,
    .metadata = context.metadata,
  };
  for (const auto& resource : context.resolved_resources) {
    std::error_code error;
    const auto relative = std::filesystem::relative(resource, policy_.root, error);
    capability_request.resources.push_back(
      error ? resource.generic_string() : relative.generic_string());
  }

  audit::audit_event evaluated {
    .module = "filesystem",
    .name = "policy_evaluated",
    .id = context.id,
    .subject_id = context.id,
    .outcome = audit::audit_event_outcome::allowed,
    .attributes = context.metadata,
  };
  safely_publish(audit_, evaluated);

  if (!context.approval_required) return true;
  if (!approvals_) {
    context.failure = {
      .status = filesystem_status::approval_denied,
      .error_message = "approval required but no approval service is configured",
    };
    return false;
  }
  approval::approval_request request {
    .id = context.id,
    .summary = capability_request.summary,
    .capabilities = { std::move(capability_request) },
    .metadata = context.metadata,
  };
  approval::approval_decision decision;
  try { decision = approvals_->decide(request); }
  catch (const std::exception& error) {
    context.failure = {
      .status = filesystem_status::approval_denied,
      .error_message = std::string("approval service failed: ") + error.what(),
    };
    return false;
  }
  catch (...) {
    context.failure = {
      .status = filesystem_status::approval_denied,
      .error_message = "approval service failed with an unknown exception",
    };
    return false;
  }
  if (decision.kind != approval::approval_decision_kind::approved) {
    context.failure = {
      .status = filesystem_status::approval_denied,
      .error_message = decision.reason.empty() ? "approval denied" : decision.reason,
    };
    return false;
  }
  audit::audit_event approved {
    .module = "filesystem",
    .name = "approval_approved",
    .id = context.id,
    .subject_id = context.id,
    .outcome = audit::audit_event_outcome::approved,
    .attributes = context.metadata,
  };
  safely_publish(audit_, approved);
  return true;
}

filesystem_result filesystem_runtime::finish(
  operation_context& context,
  filesystem_result result) const {
  const auto relativize = [&](std::filesystem::path& path) {
    if (path.empty()) return;
    const auto normalized = path.lexically_normal();
    if (!path_within(normalized, policy_.root)) return;
    const auto relative = normalized.lexically_relative(policy_.root);
    if (!relative.empty()) path = relative;
  };
  relativize(result.path);
  relativize(result.destination);
  for (auto& entry : result.entries) relativize(entry.path);
  for (auto& match : result.matches) relativize(match.path);
  for (const auto& [key, value] : context.metadata) result.metadata[key] = value;

  audit::audit_event event {
    .module = "filesystem",
    .name = "operation_finished",
    .id = context.id,
    .subject_id = context.id,
    .outcome = outcome_for(result.status),
    .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - context.started),
    .attributes = context.metadata,
  };
  event.attributes["status"] = to_string(result.status);
  event.attributes["bytes_processed"] = std::to_string(result.bytes_processed);
  event.attributes["affected_items"] = std::to_string(result.affected_items);
  event.attributes["truncated"] = result.truncated ? "true" : "false";
  if (!result.error_message.empty()) event.attributes["error"] = result.error_message;
  safely_publish(audit_, event);
  return result;
}

filesystem_result filesystem_runtime::read_text(
  read_text_request request,
  std::stop_token stop_token) {
  auto context = begin_operation("read_text", { request.path }, false, false, request.metadata);
  if (!policy_.allow_read) context->failure = { .status = filesystem_status::permission_denied, .error_message = "file reads are disabled" };
  const auto path = resolve_path(request.path, *context);
  if (!path || !authorize(*context, stop_token)) return finish(*context, context->failure);
  request.path = *path;
  request.max_bytes = request.max_bytes == 0 ? policy_.max_read_bytes
    : (std::min)(request.max_bytes, policy_.max_read_bytes);
  return finish(*context, safely_invoke_backend(request.path, [&] {
    return backend_->read_text(request, stop_token);
  }));
}

filesystem_result filesystem_runtime::file_info(
  file_info_request request,
  std::stop_token stop_token) {
  auto context = begin_operation("file_info", { request.path }, false, false, request.metadata);
  if (!policy_.allow_read) context->failure = { .status = filesystem_status::permission_denied, .error_message = "file reads are disabled" };
  const auto path = resolve_path(request.path, *context);
  if (!path || !authorize(*context, stop_token)) return finish(*context, context->failure);
  request.path = *path;
  request.max_revision_bytes = request.max_revision_bytes == 0
    ? policy_.max_read_bytes
    : (std::min)(request.max_revision_bytes, policy_.max_read_bytes);
  return finish(*context, safely_invoke_backend(request.path, [&] {
    return backend_->file_info(request, stop_token);
  }));
}

filesystem_result filesystem_runtime::write_text(
  write_text_request request,
  std::stop_token stop_token) {
  auto context = begin_operation("write_text", { request.path }, true,
    policy_.require_approval_for_write, request.metadata);
  if (!policy_.allow_write) context->failure = { .status = filesystem_status::permission_denied, .error_message = "file writes are disabled" };
  if (request.content.size() > policy_.max_write_bytes) context->failure = { .status = filesystem_status::limit_exceeded, .error_message = "content exceeds max_write_bytes" };
  const auto path = resolve_path(request.path, *context);
  if (!path || !authorize(*context, stop_token)) return finish(*context, context->failure);
  request.path = *path;
  return finish(*context, safely_invoke_backend(request.path, [&] {
    return backend_->write_text(request, stop_token);
  }));
}

filesystem_result filesystem_runtime::replace_text(
  replace_text_request request,
  std::stop_token stop_token) {
  auto context = begin_operation("replace_text", { request.path }, true,
    policy_.require_approval_for_write, request.metadata);
  if (!policy_.allow_write) context->failure = { .status = filesystem_status::permission_denied, .error_message = "file writes are disabled" };
  if (request.old_text.empty() ||
      (!request.replace_all && request.expected_replacements == 0)) {
    context->failure = {
      .status = filesystem_status::invalid_request,
      .error_message = "old_text and, unless replace_all is set, a positive expected_replacements are required",
    };
  }
  if (request.old_text.size() > policy_.max_write_bytes ||
      request.new_text.size() > policy_.max_write_bytes) {
    context->failure = {
      .status = filesystem_status::limit_exceeded,
      .error_message = "replacement text exceeds max_write_bytes",
    };
  }
  const auto path = resolve_path(request.path, *context);
  if (!path || !authorize(*context, stop_token)) return finish(*context, context->failure);
  request.path = *path;
  auto current = safely_invoke_backend(request.path, [&] {
    return backend_->read_text({
      .path = request.path,
      .max_bytes = policy_.max_write_bytes,
    }, stop_token);
  });
  if (!current.successful()) return finish(*context, std::move(current));
  request.max_result_bytes = policy_.max_write_bytes;
  return finish(*context, safely_invoke_backend(request.path, [&] {
    return backend_->replace_text(request, stop_token);
  }));
}

filesystem_result filesystem_runtime::list_directory(
  list_directory_request request,
  std::stop_token stop_token) {
  auto context = begin_operation("list_directory", { request.path }, false, false, request.metadata);
  if (!policy_.allow_read) context->failure = { .status = filesystem_status::permission_denied, .error_message = "file reads are disabled" };
  const auto path = resolve_path(request.path, *context);
  if (!path || !authorize(*context, stop_token)) return finish(*context, context->failure);
  request.path = *path;
  request.max_entries = request.max_entries == 0 ? policy_.max_directory_entries : (std::min)(request.max_entries, policy_.max_directory_entries);
  request.max_depth = request.max_depth == 0 ? policy_.max_search_depth : (std::min)(request.max_depth, policy_.max_search_depth);
  return finish(*context, safely_invoke_backend(request.path, [&] {
    return backend_->list_directory(request, stop_token);
  }));
}

filesystem_result filesystem_runtime::glob(
  glob_request request,
  std::stop_token stop_token) {
  auto context = begin_operation("glob", { request.path }, false, false, request.metadata);
  if (!policy_.allow_read) context->failure = { .status = filesystem_status::permission_denied, .error_message = "file reads are disabled" };
  if (request.pattern.empty()) {
    context->failure = {
      .status = filesystem_status::invalid_request,
      .error_message = "glob pattern must not be empty",
    };
  }
  else if (request.pattern.size() > policy_.max_pattern_bytes) {
    context->failure = {
      .status = filesystem_status::limit_exceeded,
      .error_message = "glob pattern is too large",
    };
  }
  const auto path = resolve_path(request.path, *context);
  if (!path || !authorize(*context, stop_token)) return finish(*context, context->failure);
  request.path = *path;
  request.max_entries = request.max_entries == 0 ? policy_.max_directory_entries : (std::min)(request.max_entries, policy_.max_directory_entries);
  request.max_depth = request.max_depth == 0 ? policy_.max_search_depth : (std::min)(request.max_depth, policy_.max_search_depth);
  return finish(*context, safely_invoke_backend(request.path, [&] {
    return backend_->glob(request, stop_token);
  }));
}

filesystem_result filesystem_runtime::search_text(
  search_text_request request,
  std::stop_token stop_token) {
  auto context = begin_operation("search_text", { request.path }, false, false, request.metadata);
  if (!policy_.allow_read) context->failure = { .status = filesystem_status::permission_denied, .error_message = "file reads are disabled" };
  if (request.query.empty() || request.file_pattern.empty()) {
    context->failure = {
      .status = filesystem_status::invalid_request,
      .error_message = "search query and file pattern must not be empty",
    };
  }
  else if (request.query.size() > policy_.max_pattern_bytes ||
           request.file_pattern.size() > policy_.max_pattern_bytes) {
    context->failure = {
      .status = filesystem_status::limit_exceeded,
      .error_message = "search query or file pattern is too large",
    };
  }
  const auto path = resolve_path(request.path, *context);
  if (!path || !authorize(*context, stop_token)) return finish(*context, context->failure);
  request.path = *path;
  request.max_depth = request.max_depth == 0 ? policy_.max_search_depth : (std::min)(request.max_depth, policy_.max_search_depth);
  request.max_files = request.max_files == 0 ? policy_.max_directory_entries : (std::min)(request.max_files, policy_.max_directory_entries);
  request.max_results = request.max_results == 0 ? policy_.max_search_results : (std::min)(request.max_results, policy_.max_search_results);
  request.max_file_bytes = request.max_file_bytes == 0 ? policy_.max_search_file_bytes : (std::min)(request.max_file_bytes, policy_.max_search_file_bytes);
  request.max_total_bytes = request.max_total_bytes == 0 ? policy_.max_search_total_bytes : (std::min)(request.max_total_bytes, policy_.max_search_total_bytes);
  request.max_output_bytes = request.max_output_bytes == 0 ? policy_.max_search_output_bytes : (std::min)(request.max_output_bytes, policy_.max_search_output_bytes);
  return finish(*context, safely_invoke_backend(request.path, [&] {
    return backend_->search_text(request, stop_token);
  }));
}

filesystem_result filesystem_runtime::create_directory(
  create_directory_request request,
  std::stop_token stop_token) {
  auto context = begin_operation("create_directory", { request.path }, true,
    policy_.require_approval_for_write, request.metadata);
  if (!policy_.allow_create_directory) context->failure = { .status = filesystem_status::permission_denied, .error_message = "directory creation is disabled" };
  const auto path = resolve_path(request.path, *context);
  if (!path || !authorize(*context, stop_token)) return finish(*context, context->failure);
  request.path = *path;
  return finish(*context, safely_invoke_backend(request.path, [&] {
    return backend_->create_directory(request, stop_token);
  }));
}

filesystem_result filesystem_runtime::copy_path(
  transfer_path_request request,
  std::stop_token stop_token) {
  auto context = begin_operation("copy_path", { request.source, request.destination }, true,
    policy_.require_approval_for_write, request.metadata);
  if (!policy_.allow_copy) context->failure = { .status = filesystem_status::permission_denied, .error_message = "copy operations are disabled" };
  const auto source = resolve_path(request.source, *context);
  const auto destination = resolve_path(request.destination, *context);
  if (!source || !destination || !authorize(*context, stop_token)) return finish(*context, context->failure);
  request.source = *source; request.destination = *destination;
  request.max_entries = request.max_entries == 0 ? policy_.max_directory_entries : (std::min)(request.max_entries, policy_.max_directory_entries);
  request.max_bytes = request.max_bytes == 0 ? policy_.max_copy_bytes : (std::min)(request.max_bytes, policy_.max_copy_bytes);
  return finish(*context, safely_invoke_backend(request.source, [&] {
    return backend_->copy_path(request, stop_token);
  }));
}

filesystem_result filesystem_runtime::move_path(
  transfer_path_request request,
  std::stop_token stop_token) {
  auto context = begin_operation("move_path", { request.source, request.destination }, true,
    policy_.require_approval_for_move, request.metadata);
  if (!policy_.allow_move) context->failure = { .status = filesystem_status::permission_denied, .error_message = "move operations are disabled" };
  const auto source = resolve_path(request.source, *context);
  const auto destination = resolve_path(request.destination, *context);
  if (!source || !destination || !authorize(*context, stop_token)) return finish(*context, context->failure);
  request.source = *source; request.destination = *destination;
  return finish(*context, safely_invoke_backend(request.source, [&] {
    return backend_->move_path(request, stop_token);
  }));
}

filesystem_result filesystem_runtime::remove_path(
  remove_path_request request,
  std::stop_token stop_token) {
  auto context = begin_operation("remove_path", { request.path }, true,
    policy_.require_approval_for_remove, request.metadata);
  if (!policy_.allow_remove) context->failure = { .status = filesystem_status::permission_denied, .error_message = "remove operations are disabled" };
  const auto path = resolve_path(request.path, *context);
  if (path && *path == policy_.root) context->failure = { .status = filesystem_status::permission_denied, .error_message = "removing the configured root is forbidden" };
  if (!path || !authorize(*context, stop_token)) return finish(*context, context->failure);
  request.path = *path;
  request.max_entries = request.max_entries == 0 ? policy_.max_directory_entries : (std::min)(request.max_entries, policy_.max_directory_entries);
  return finish(*context, safely_invoke_backend(request.path, [&] {
    return backend_->remove_path(request, stop_token);
  }));
}

const filesystem_policy& filesystem_runtime::policy() const noexcept { return policy_; }
const filesystem_backend* filesystem_runtime::backend() const noexcept { return backend_.get(); }

void filesystem_runtime::audit_tool_rejection(
  const std::string& tool_name,
  const std::string& reason,
  const std::map<std::string, std::string>& attributes) {
  audit::audit_event event {
    .module = "filesystem",
    .name = "tool_rejected",
    .id = "filesystem-" + std::to_string(next_operation_id_.fetch_add(1)),
    .outcome = audit::audit_event_outcome::denied,
    .attributes = attributes,
  };
  event.subject_id = event.id;
  event.attributes["tool_name"] = tool_name;
  event.attributes["reason"] = reason;
  safely_publish(audit_, event);
}

} // namespace wuwe::agent::filesystem
