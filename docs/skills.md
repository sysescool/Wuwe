---
id: skills
title: Skills
description: Package, resolve, and activate reusable agent capabilities without bypassing tools, policy, or trust boundaries.
---

# Skills

The Skills module packages reusable agent instructions and resource declarations behind a strict, versioned contract. It is an assembly layer: it selects immutable package versions, verifies declared resources, checks runtime requirements, and projects the resulting activation into LLM, Planning, Knowledge, Multi-Agent, A2A, or MCP integration points.

A Skill does not execute code, grant a capability, approve an operation, or replace an Agent. Tools remain the executable contract; Agents remain runtime identities and workers; Skills describe reusable behavior and the requirements needed to assemble it.

## Lifecycle

```text
directory -> verified immutable package -> registry snapshot
          -> deterministic dependency resolution -> activation
          -> explicit adapter -> existing governed runtime
```

The main public types are available through `<wuwe/agent/skills/skills.hpp>`:

- `directory_skill_loader` validates a local package and freezes its bytes;
- `skill_registry` publishes copy-on-write immutable snapshots;
- `skill_resolver` selects one exact SemVer package per Skill ID;
- `skill_activator` checks Tool and Knowledge availability and produces a stable activation fingerprint;
- `apply_skill_activation`, `scoped_tool_provider`, and the Knowledge adapter project an activation into existing runtime contracts.

Adapters for Multi-Agent, A2A, and MCP are separate headers because those projections are optional and intentionally lossy.

## Package format

Each package is a directory containing an authoritative `skill.json`. `SKILL.md` has no implicit authority; it is an ordinary declared resource.

```json
{
  "schema_version": 1,
  "id": "com.example.review",
  "version": "1.0.0",
  "name": "Review",
  "description": "Review an artifact against an engineering rubric.",
  "instructions": ["main"],
  "resources": [
    {
      "id": "main",
      "path": "SKILL.md",
      "kind": "instructions",
      "media_type": "text/markdown",
      "size": 190,
      "sha256": "64-lowercase-hex-characters"
    }
  ],
  "requires": {
    "skills": [{"id": "com.example.base", "version": "^1.0.0"}],
    "tools": [{"name": "filesystem.read", "exact_version": "1"}],
    "capabilities": [],
    "knowledge": []
  }
}
```

The parser rejects duplicate JSON keys, unknown contract fields, unsupported schema versions, invalid SemVer, unsupported JSON Schema assertions, invalid resource paths, duplicate identities, and non-canonical hashes. Tool versions are opaque exact provider versions; only Skill dependencies use SemVer.

All files other than `skill.json` must be declared. Every resource has an exact byte size and SHA-256. The package digest covers the raw manifest and verified resources deterministically.

Programmatically constructed embedded packages receive a deterministic digest
from their canonical manifest and verified resource bytes. Directory and future
external sources must supply the digest established by their source verifier.

## Loading and integrity

```cpp
namespace skills = wuwe::agent::skills;

skills::directory_skill_loader loader({
  .root = std::filesystem::absolute("application-skills"),
  .max_resource_bytes = 1024 * 1024,
  .max_package_bytes = 8 * 1024 * 1024,
});

auto loaded = loader.load("review");
if (!loaded) {
  // Inspect loaded.error, loaded.message, and loaded.diagnostics.
}
```

Package paths are root-relative portable paths. Absolute paths, traversal, backslashes, Windows device/reserved names, symlinks and reparse points, case-folding collisions, undeclared files, and hard-linked resources are rejected by default. Resource count, per-file size, total size, path depth, and manifest limits bound allocation and traversal.

The portable directory loader validates paths and reads an immutable in-memory snapshot, but no path-based API can eliminate every time-of-check/time-of-use race against a hostile process that can mutate the same directory concurrently. Deploy packages from a host-controlled read-only root, or stage them into immutable storage before loading. Wuwe 1.0.0 does not include archive extraction or remote installation.

SHA-256 establishes byte integrity, not publisher authenticity. Local directories default to `retrieved_untrusted`. A host may explicitly configure a stronger trust level only after applying its own source policy. Manifest metadata cannot promote trust.

## Registration and resolution

```cpp
skills::skill_registry registry;
registry.register_package(loaded.package);

auto resolution = skills::skill_resolver().resolve(registry.snapshot(), {
  .roots = {{
    .id = "com.example.review",
    .version = skills::version_requirement::parse("^1.0.0"),
  }},
});
```

Registry reads are lock-free snapshots; writes publish a new immutable map. A snapshot remains stable if the live registry later changes. Resolution is deterministic, dependency-first, bounded, cycle-aware, and backtracks when the highest candidate conflicts with another constraint. Prerelease packages are selected only when a requirement explicitly names a prerelease.

Registering the same `(id, version, digest)` is idempotent. Different content at
the same ID and version is rejected unless the host deliberately supplies
`skill_registration_policy::replace`; existing snapshots and activations still
retain the package they already hold.

An activation consumes the exact resolved packages. It never performs a second floating version lookup, so a run cannot silently drift after resolution.

## Activation and runtime boundaries

```cpp
auto activation = skills::skill_activator().activate({
  .resolution = resolution,
  .catalog = {
    .tools = provider.descriptors(),
    .knowledge_sources = {"engineering-policy"},
  },
  .context = execution_context,
});
```

Activation checks required Tools by name and exact version, checks declared Knowledge sources, merges capability declarations conservatively, enforces package and context limits, and returns a SHA-256 activation fingerprint. Capability declarations are admission information only. Every actual tool or execution call still passes through its authoritative Tool Contract, capability policy, approval service, cancellation, audit, and backend enforcement.

Use `scoped_tool_provider` around the real provider. Filtering only the model-visible Tool list is insufficient because application code could otherwise invoke a non-activated Tool directly.

Trusted instructions may be projected as system messages. Untrusted instructions are escaped and added as bounded user data with an explicit instruction-isolation warning. Skill context has its own `llm_context_source::skill` accounting and `.limits.skills` budget.

Knowledge resources are not indexed automatically. `knowledge_documents_from_activation()` is an explicit projection step that retains package and resource provenance.

Script resources are inert package data. The Skills module never executes them. A future host workflow may choose to stage verified bytes and submit them to the existing Execution Runtime, but that is a separate policy and approval boundary.

## Protocol projections

- Multi-Agent adapters convert a package descriptor into routing metadata. They do not transfer package ownership or activation state.
- A2A publication exposes a bounded Agent Card summary. A remote advertisement is untrusted and not locally activatable.
- MCP integration requires an explicit `(server, tool, exposed name)` allowlist. It never imports every tool from a server automatically.

## 1.0 scope

Wuwe 1.0.0 supports embedded packages and strict local-directory loading. It intentionally does not provide a marketplace, remote download/update, archive extraction, signature or publisher verification, automatic script execution, or automatic MCP/A2A capability import. Those features require separate supply-chain and governance designs and are not implied by the stable Skills API.
