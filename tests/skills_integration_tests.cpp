#include <map>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>

#include <wuwe/agent/a2a/skills_adapter.hpp>
#include <wuwe/agent/llm/context_budget.hpp>
#include <wuwe/agent/mcp/skills_adapter.hpp>
#include <wuwe/agent/multi_agent/skills_adapter.hpp>
#include <wuwe/agent/runtime/llm_continuation.hpp>
#include <wuwe/common/sha256.hpp>

using namespace wuwe;
using namespace wuwe::agent;

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

skills::skill_package_ptr package() {
  const auto content = std::string("Review the requested artifact.");
  const auto hash = common::sha256_hex(content);
  skills::skill_resource_descriptor resource {
    .id = "instructions",
    .path = "SKILL.md",
    .kind = skills::skill_resource_kind::instructions,
    .media_type = "text/markdown",
    .size = content.size(),
    .sha256 = hash,
  };
  skills::skill_manifest manifest {
    .descriptor = {
      .id = "com.example.review",
      .name = "Review",
      .description = "Review an artifact against a stable rubric",
      .version = skills::semantic_version::parse("1.2.3"),
      .tags = { "review", "quality" },
      .examples = { "Review this patch" },
      .input_modes = { "text/plain" },
      .output_modes = { "text/markdown" },
    },
    .instruction_resources = { "instructions" },
    .resources = { resource },
  };
  return std::make_shared<const skills::skill_package>(std::move(manifest),
    skills::skill_provenance {
      .origin = skills::skill_origin_kind::embedded,
      .source_uri = "embedded:review",
      .content_sha256 = hash,
      .trust = core::content_trust_level::application_trusted,
    },
    std::map<std::string, skills::skill_resource> {
      { "instructions", { .descriptor = resource, .content = content, .sha256 = hash } },
    });
}

void multi_agent_projection_remains_a_routing_summary() {
  auto value = package();
  auto projected = multi_agent::agent_skill_from_package(*value);
  require(projected.id == value->descriptor().id &&
            projected.metadata.at("wuwe.skill.version") == "1.2.3" &&
            projected.metadata.at("wuwe.skill.digest") == value->provenance().content_sha256,
    "multi-agent projection carries stable identity without package ownership");

  multi_agent::agent_descriptor agent {
    .id = "reviewer",
    .name = "Reviewer",
  };
  multi_agent::attach_skill(agent, *value);
  bool duplicate_rejected = false;
  try {
    multi_agent::attach_skill(agent, *value);
  }
  catch (const std::invalid_argument&) {
    duplicate_rejected = true;
  }
  require(agent.skills.size() == 1 && duplicate_rejected,
    "agent skill attachment rejects ambiguous duplicate routing identities");
}

void a2a_publication_is_lossy_and_remote_advertisements_are_not_activatable() {
  auto value = package();
  a2a::agent_card card {
    .name = "Remote reviewer",
    .description = "Remote A2A review service",
    .url = "https://agents.example/reviewer",
  };
  a2a::publish_skill(card, *value);
  require(
    card.skills.size() == 1 &&
      card.metadata.at("wuwe").at("skills").at(value->descriptor().id).at("version") == "1.2.3",
    "A2A publication exposes a protocol summary and bounded Wuwe extension metadata");

  const auto external = a2a::external_skill_reference_from_card(card, card.skills.front());
  require(!external.activatable &&
            external.trust == core::content_trust_level::retrieved_untrusted &&
            external.descriptor.version == skills::semantic_version::parse("1.2.3"),
    "remote Agent Card skills remain untrusted advertisements rather than local packages");
}

void context_budget_accounts_for_skill_instructions_separately() {
  llm_request request;
  request.messages.push_back({
    .role = "user",
    .content = std::string(400, 's'),
    .context_source = llm_context_source::skill,
  });
  request.messages.push_back({ .role = "user", .content = "conversation" });
  llm::context_budget_manager manager;
  const auto fitted = manager.fit(std::move(request),
    {
      .context_window_tokens = 256,
      .reserved_output_tokens = 32,
      .limits = { .skills = 20 },
    });
  require(fitted && fitted.report.before.skills > 20 && fitted.report.after.skills <= 20,
    "skills have an independent, enforceable context-budget component");
}

void durable_llm_continuations_preserve_skill_context_identity() {
  static_assert(static_cast<int>(llm_context_source::tool_result) == 5);
  static_assert(static_cast<int>(llm_context_source::other) == 6);
  static_assert(static_cast<int>(llm_context_source::skill) == 7);
  runtime::llm_tool_continuation continuation;
  continuation.request.messages.push_back({
    .role = "user",
    .content = "bounded skill context",
    .context_source = llm_context_source::skill,
  });
  continuation.request.context_budget = llm_context_budget {
    .context_window_tokens = 1024,
    .limits = { .skills = 128 },
  };
  const auto restored =
    runtime::llm_continuation_from_json(runtime::llm_continuation_to_json(continuation));
  require(restored.request.messages.front().context_source == llm_context_source::skill &&
            restored.request.context_budget->limits.skills == 128,
    "durable LLM continuation serialization preserves Skill context identity and budget");
}

void mcp_skill_bindings_are_explicit_and_invokable() {
#ifdef WUWE_MCP_STDIO_EXAMPLE_PATH
  mcp::mcp_host_runtime runtime;
  runtime.add_server({
    .id = "skills",
    .command = { .command = WUWE_MCP_STDIO_EXAMPLE_PATH },
    .client_info = { .name = "wuwe-skills-test", .version = "1.0" },
  });
  runtime.start_all();

  mcp::mcp_skill_tool_provider provider(runtime,
    { {
      .server_id = "skills",
      .tool_name = "echo_text",
      .exposed_name = "review_echo",
    } });
  const auto descriptors = provider.descriptors();
  require(descriptors.size() == 1 && descriptors.front().name == "review_echo" &&
            descriptors.front().approval == tools::tool_approval_mode::policy,
    "MCP Skills expose only explicit bindings through a governed Tool Contract");
  require(provider.invoke("review_echo", R"({"text":"hello skills"})").content == "hello skills",
    "an explicitly bound MCP Skill tool reaches the selected server tool");
  require(provider.invoke("unbound", "{}").error_category == tools::tool_error_category::not_found,
    "unbound MCP tools cannot be invoked through the Skill provider");

  std::stop_source stop;
  stop.request_stop();
  require(provider
              .invoke({
                .name = "review_echo",
                .arguments_json = R"({"text":"cancelled"})",
                .stop_token = stop.get_token(),
              })
              .error_category == tools::tool_error_category::cancelled,
    "MCP Skill bindings honor cancellation before dispatch");
  runtime.stop_all();
#endif
}

} // namespace

int main() {
  multi_agent_projection_remains_a_routing_summary();
  a2a_publication_is_lossy_and_remote_advertisements_are_not_activatable();
  context_budget_accounts_for_skill_instructions_separately();
  durable_llm_continuations_preserve_skill_context_identity();
  mcp_skill_bindings_are_explicit_and_invokable();
}
