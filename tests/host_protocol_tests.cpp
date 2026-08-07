#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <wuwe/agent/approval/approval_context.hpp>
#include <wuwe/agent/audit/audit_context.hpp>
#include <wuwe/agent/core/execution_observability.hpp>
#include <wuwe/agent/host/host.hpp>
#include <wuwe/agent/knowledge/knowledge_context.hpp>
#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/agent/runtime/runtime.hpp>

namespace {

namespace host = wuwe::agent::host;
namespace runtime = wuwe::agent::runtime;

void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

class recording_host_service : public host::agent_host_service {
public:
  host::host_call_context last_call;
  host::create_run_request last_create;
  int calls {};
  bool conflict_next {};

  host::host_result<host::run_submission> create_run(
    const host::host_call_context& call, const host::create_run_request& request) override {
    ++calls;
    last_call = call;
    last_create = request;
    return host::host_result<host::run_submission>::success({
      .run_id = request.context.run_id,
      .revision = 2,
      .status = runtime::agent_run_status::running,
    });
  }

  host::host_result<host::run_view> get_run(
    const host::host_call_context&, const host::get_run_request& request) override {
    ++calls;
    if (conflict_next) {
      conflict_next = false;
      return host::host_result<host::run_view>::failure({
        .code = host::host_error_code::conflict,
        .message = "revision conflict",
      });
    }
    host::run_view record {
      .run_id = request.run_id,
      .revision = 3,
      .status = runtime::agent_run_status::running,
    };
    record.context.run_id = request.run_id;
    return host::host_result<host::run_view>::success(std::move(record));
  }

  host::host_result<host::run_submission> cancel_run(
    const host::host_call_context&, const host::cancel_run_request& request) override {
    ++calls;
    return host::host_result<host::run_submission>::success({
      .run_id = request.run_id,
      .revision = request.expected_revision + 1,
      .status = runtime::agent_run_status::cancelled,
    });
  }

  host::host_result<host::run_submission> resolve_approval(
    const host::host_call_context&, const host::resolve_approval_request& request) override {
    ++calls;
    return host::host_result<host::run_submission>::success({
      .run_id = request.run_id,
      .revision = request.expected_revision + 1,
      .status = request.resolution == runtime::approval_resolution::approved
                  ? runtime::agent_run_status::waiting_for_approval
                  : runtime::agent_run_status::failed,
    });
  }

  host::host_result<host::run_submission> resume_run(
    const host::host_call_context&, const host::resume_run_request& request) override {
    ++calls;
    return host::host_result<host::run_submission>::success({
      .run_id = request.run_id,
      .revision = request.expected_revision + 1,
      .status = runtime::agent_run_status::running,
    });
  }

  host::host_result<host::event_page> list_events(
    const host::host_call_context&, const host::list_events_request& request) override {
    ++calls;
    return host::host_result<host::event_page>::success({
      .events = {
        { .run_id = request.run_id, .sequence = request.after_sequence + 1,
          .type = "first", .status = runtime::agent_run_status::running },
        { .run_id = request.run_id, .sequence = request.after_sequence + 2,
          .type = "second", .status = runtime::agent_run_status::running },
      },
      .next_sequence = request.after_sequence + 2,
      .has_more = true,
    });
  }
};

void host_protocol_is_versioned_typed_and_transport_neutral() {
  recording_host_service service;
  host::host_dispatcher dispatcher(service);
  wuwe::agent::core::agent_execution_context context {
    .run_id = "run-1",
    .trace_id = "trace-1",
    .tenant_id = "tenant-1",
    .user_id = "user-1",
  };
  host::host_request_envelope request {
    .call = {
      .request_id = "request-1",
      .idempotency_key = "create-1",
      .metadata = { { "transport", "test" } },
    },
    .operation = host::host_operation::create_run,
    .body = host::create_run_request_to_json({
      .context = context,
      .input = { { "prompt", "hello" } },
      .metadata = { { "agent_type", "general" } },
    }),
  };

  const auto encoded = host::host_request_to_json(request);
  const auto response = dispatcher.dispatch_json(encoded);
  const auto decoded_response = host::host_response_from_json(response);
  const auto submission = host::run_submission_from_json(decoded_response.body);
  require(response.at("ok").get<bool>(), "valid host request should succeed");
  require(response.at("protocolVersion") == std::string(host::default_protocol_version) &&
            response.at("requestId") == "request-1" && submission.run_id == "run-1" &&
            submission.revision == 2,
    "host response should preserve protocol correlation and typed result");
  require(service.calls == 1 && service.last_call.idempotency_key == "create-1" &&
            service.last_create.context.tenant_id == "tenant-1" &&
            service.last_create.input.at("prompt") == "hello",
    "dispatcher should decode typed request without transport assumptions");
}

void host_protocol_fails_closed_before_service_dispatch() {
  recording_host_service service;
  host::host_dispatcher dispatcher(service);
  const auto unsupported = dispatcher.dispatch_json({
    { "protocolVersion", "2099-01-01" },
    { "requestId", "request-2" },
    { "operation", "get_run" },
    { "body", { { "runId", "run-1" } } },
  });
  require(!unsupported.at("ok").get<bool>() &&
            unsupported.at("error").at("code") == "unsupported_protocol_version" &&
            unsupported.at("error").at("details").at("supportedVersions").size() == 1 &&
            service.calls == 0,
    "unsupported protocol version should fail before service invocation");

  host::host_request_envelope typed_unsupported {
    .call = {
      .protocol_version = "2099-01-01",
      .request_id = "request-typed-version",
    },
    .operation = host::host_operation::get_run,
    .body = { { "runId", "run-1" } },
  };
  const auto typed_response = dispatcher.dispatch(typed_unsupported);
  require(typed_response.error &&
            typed_response.error->code == host::host_error_code::unsupported_protocol_version &&
            service.calls == 0,
    "typed dispatch should preserve unsupported-version semantics");

  const auto missing_idempotency = dispatcher.dispatch_json({
    { "protocolVersion", host::default_protocol_version },
    { "requestId", "request-3" },
    { "operation", "cancel_run" },
    { "body", { { "runId", "run-1" }, { "expectedRevision", 2 } } },
  });
  require(!missing_idempotency.at("ok").get<bool>() &&
            missing_idempotency.at("error").at("code") == "invalid_request" && service.calls == 0,
    "mutating host operations should require an idempotency key");

  const auto malformed_metadata = dispatcher.dispatch_json({
    { "protocolVersion", host::default_protocol_version },
    { "requestId", "request-malformed-metadata" },
    { "operation", "get_run" },
    { "metadata", 7 },
    { "body", { { "runId", "run-1" } } },
  });
  require(!malformed_metadata.at("ok").get<bool>() &&
            malformed_metadata.at("error").at("code") == "invalid_request" && service.calls == 0,
    "malformed envelope field types should remain client errors");
}

void host_protocol_preserves_service_errors_and_event_cursors() {
  recording_host_service service;
  host::host_dispatcher dispatcher(service);
  service.conflict_next = true;
  const auto conflict = dispatcher.dispatch_json({
    { "protocolVersion", host::default_protocol_version },
    { "requestId", "request-4" },
    { "operation", "get_run" },
    { "body", { { "runId", "run-1" } } },
  });
  require(conflict.at("error").at("code") == "conflict",
    "dispatcher should preserve typed service errors");

  const auto events = dispatcher.dispatch_json({
    { "protocolVersion", host::default_protocol_version },
    { "requestId", "request-5" },
    { "operation", "list_events" },
    { "body",
      {
        { "runId", "run-1" },
        { "afterSequence", 7 },
        { "limit", 10 },
      } },
  });
  require(events.at("body").at("events").size() == 2 && events.at("body").at("nextSequence") == 9 &&
            events.at("body").at("hasMore").get<bool>(),
    "event page should carry a stable exclusive replay cursor");
}

void host_protocol_projects_public_run_state_without_runtime_secrets() {
  runtime::agent_run_record internal {
    .id = "run-secret",
    .revision = 4,
    .status = runtime::agent_run_status::waiting_for_approval,
  };
  internal.context.run_id = internal.id;
  internal.context.metadata = {
    { "region", "ap" },
    { "accessToken", "context-secret" },
  };
  internal.suspension = runtime::agent_run_suspension {
    .approval_id = "approval-secret",
    .continuation_token = "bearer-secret",
    .tool_call_id = "tool-call-secret",
    .tool_name = "write_config",
    .continuation = { { "private", "runtime-only" } },
    .metadata = { { "api-key", "approval-secret" } },
  };
  internal.admitted_tool_results.emplace("previous-call",
    runtime::admitted_tool_result {
      .tool_call_id = "previous-call",
      .idempotency_key = "internal-idempotency",
      .tool_name = "read_config",
    });

  const auto encoded = host::run_view_to_json(host::run_view_from_runtime(internal));
  const auto serialized = encoded.dump();
  require(encoded.at("runId") == internal.id &&
            encoded.at("approval").at("approvalId") == "approval-secret" &&
            !encoded.at("context").at("metadata").contains("accessToken") &&
            !encoded.at("approval").at("metadata").contains("api-key") &&
            serialized.find("bearer-secret") == std::string::npos &&
            serialized.find("runtime-only") == std::string::npos &&
            serialized.find("internal-idempotency") == std::string::npos,
    "host run projection must not expose runtime continuation or admission secrets");
}

class throwing_host_service final : public recording_host_service {
public:
  host::host_result<host::run_view> get_run(
    const host::host_call_context&, const host::get_run_request&) override {
    throw std::invalid_argument("database-internal-detail");
  }
};

void host_protocol_separates_client_and_service_failures() {
  throwing_host_service service;
  host::host_dispatcher dispatcher(service);
  const auto response = dispatcher.dispatch_json({
    { "protocolVersion", host::default_protocol_version },
    { "requestId", "request-service-failure" },
    { "operation", "get_run" },
    { "body", { { "runId", "run-1" } } },
  });
  require(response.at("error").at("code") == "internal" &&
            response.dump().find("database-internal-detail") == std::string::npos,
    "service exceptions must be internal and must not expose implementation details");
}

class internal_error_host_service final : public recording_host_service {
public:
  host::host_result<host::run_view> get_run(
    const host::host_call_context&, const host::get_run_request&) override {
    return host::host_result<host::run_view>::failure({
      .code = host::host_error_code::internal,
      .message = "database-password=secret",
      .details = { { "continuationToken", "bearer-secret" } },
    });
  }
};

void host_protocol_sanitizes_typed_internal_errors() {
  internal_error_host_service service;
  host::host_dispatcher dispatcher(service);
  const auto response = dispatcher.dispatch_json({
    { "protocolVersion", host::default_protocol_version },
    { "requestId", "request-internal-error" },
    { "operation", "get_run" },
    { "body", { { "runId", "run-1" } } },
  });
  require(response.at("error").at("code") == "internal" &&
            response.at("error").at("message") == "agent host service operation failed" &&
            response.at("error").at("details").empty() &&
            response.dump().find("bearer-secret") == std::string::npos &&
            response.dump().find("database-password") == std::string::npos,
    "typed internal service errors must not expose messages or details");
}

void host_approval_operations_never_serialize_runtime_tokens() {
  const auto resolve = host::resolve_approval_request_to_json({
    .run_id = "run-approval",
    .expected_revision = 3,
    .approval_id = "approval-3",
    .resolution = runtime::approval_resolution::approved,
  });
  const auto resume = host::resume_run_request_to_json({
    .run_id = "run-approval",
    .expected_revision = 4,
    .approval_id = "approval-3",
  });
  require(resolve.at("approvalId") == "approval-3" && resume.at("approvalId") == "approval-3" &&
            resolve.dump().find("continuationToken") == std::string::npos &&
            resume.dump().find("continuationToken") == std::string::npos,
    "Host approval operations should expose approval identity, never runtime tokens");
}

class invalid_event_host_service final : public recording_host_service {
public:
  host::host_result<host::event_page> list_events(
    const host::host_call_context&, const host::list_events_request& request) override {
    ++calls;
    return host::host_result<host::event_page>::success({
      .events = {
        { .run_id = request.run_id, .sequence = request.after_sequence,
          .type = "duplicate", .status = runtime::agent_run_status::running },
      },
      .next_sequence = request.after_sequence,
    });
  }
};

void host_protocol_rejects_invalid_service_event_pages() {
  invalid_event_host_service service;
  host::host_dispatcher dispatcher(service);
  const auto response = dispatcher.dispatch_json({
    { "protocolVersion", host::default_protocol_version },
    { "requestId", "request-invalid-page" },
    { "operation", "list_events" },
    { "body",
      {
        { "runId", "run-1" },
        { "afterSequence", 7 },
        { "limit", 10 },
      } },
  });
  require(!response.at("ok").get<bool>() && response.at("error").at("code") == "internal",
    "dispatcher should reject duplicate or stale event sequences from services");
}

void execution_context_projection_is_authoritative_and_secret_safe() {
  wuwe::agent::core::agent_execution_context context {
    .run_id = "run-7",
    .trace_id = "trace-7",
    .request_id = "request-7",
    .tenant_id = "tenant-7",
    .user_id = "user-7",
    .workspace_id = "workspace-7",
    .metadata = {
      { "region", "ap" }, { "user_id", "spoofed" },
      { "capability_token", "must-not-leak" },
    },
  };
  std::map<std::string, std::string> attributes {
    { "tenant_id", "spoofed" },
    { "operation", "read" },
  };
  wuwe::agent::core::apply_execution_context_attributes(attributes, context);
  require(attributes.at("tenant_id") == "tenant-7" && attributes.at("user_id") == "user-7" &&
            attributes.at("context.metadata.user_id") == "spoofed" &&
            !attributes.contains("context.metadata.capability_token") &&
            attributes.at("operation") == "read",
    "context identifiers should be authoritative and metadata namespaced");
  const auto serialized = wuwe::agent::core::execution_context_to_json(context);
  require(!serialized.at("metadata").contains("capability_token"),
    "execution context serialization should omit sensitive metadata by default");
  require(wuwe::agent::core::sensitive_execution_context_metadata_key("x-api-key") &&
            wuwe::agent::core::sensitive_execution_context_metadata_key("bearerToken") &&
            wuwe::agent::core::sensitive_execution_context_metadata_key("database.credentials"),
    "sensitive metadata detection should handle common naming conventions");

  const auto approval =
    wuwe::agent::approval::make_approval_request(context, "approval-1", "Approve operation");
  const auto audit = wuwe::agent::audit::make_audit_event(context, "tools", "invoke", "call-1");
  const auto event = wuwe::agent::observability::make_agent_event(context, "runtime", "started");
  const auto memory = wuwe::agent::memory::memory_scope_from_execution_context(context);
  const auto knowledge =
    wuwe::agent::knowledge::knowledge_access_from_execution_context({}, context);
  require(approval.metadata.at("workspace_id") == "workspace-7" && audit.trace_id == "trace-7" &&
            audit.subject_id == "user-7" && event.run_id == "run-7" &&
            event.request_id == "request-7" && memory.tenant_id == "tenant-7" &&
            knowledge.tenant_id == "tenant-7",
    "all framework projections should derive from one execution context");
}

class invalid_distributed_store final : public runtime::agent_run_store {
public:
  runtime::agent_run_store_capabilities capabilities() const noexcept override {
    return {
      .declared = true,
      .coordination_scope = runtime::run_store_coordination_scope::distributed,
    };
  }
  runtime::run_store_write_result create(
    runtime::agent_run_record, runtime::agent_run_event) override {
    return {};
  }
  std::optional<runtime::agent_run_record> load(const std::string&) const override {
    return std::nullopt;
  }
  runtime::run_store_write_result update(
    std::uint64_t, runtime::agent_run_record, runtime::agent_run_event) override {
    return {};
  }
  std::vector<runtime::agent_run_event> list_events(
    const std::string&, std::uint64_t) const override {
    return {};
  }
};

void run_store_capabilities_are_explicit_and_validated() {
  auto memory = std::make_shared<runtime::in_memory_agent_run_store>();
  runtime::agent_run_runtime local(memory);
  const auto capabilities = local.store_capabilities();
  require(!capabilities.durable && capabilities.optimistic_concurrency &&
            capabilities.atomic_mutations && capabilities.ordered_replay &&
            capabilities.coordination_scope == runtime::run_store_coordination_scope::process_local,
    "in-memory store should publish an accurate capability contract");

  bool rejected = false;
  try {
    runtime::agent_run_runtime invalid(std::make_shared<invalid_distributed_store>());
  }
  catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "runtime should reject internally inconsistent store capabilities");

  rejected = false;
  auto sensitive_context = wuwe::agent::core::agent_execution_context {
    .run_id = "sensitive-run",
    .metadata = { { "access_token", "must-not-persist" } },
  };
  try {
    (void)local.start(std::move(sensitive_context));
  }
  catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "durable runtime should reject sensitive execution-context metadata");
}

void run(const char* name, void (*test)()) {
  test();
  std::cout << "[PASS] " << name << '\n';
}

} // namespace

int main() {
  try {
    run("host protocol dispatch", host_protocol_is_versioned_typed_and_transport_neutral);
    run("host protocol validation", host_protocol_fails_closed_before_service_dispatch);
    run(
      "host protocol errors and cursors", host_protocol_preserves_service_errors_and_event_cursors);
    run("host protocol public run projection",
      host_protocol_projects_public_run_state_without_runtime_secrets);
    run("host protocol exception boundary", host_protocol_separates_client_and_service_failures);
    run("host protocol internal error sanitization", host_protocol_sanitizes_typed_internal_errors);
    run("host approval token boundary", host_approval_operations_never_serialize_runtime_tokens);
    run("host protocol service page validation", host_protocol_rejects_invalid_service_event_pages);
    run("execution context projection",
      execution_context_projection_is_authoritative_and_secret_safe);
    run("run store capabilities", run_store_capabilities_are_explicit_and_validated);
  }
  catch (const std::exception& error) {
    std::cerr << "[FAIL] " << error.what() << '\n';
    return 1;
  }
  return 0;
}
