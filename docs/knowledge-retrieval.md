---
id: knowledge-retrieval
title: Knowledge and RAG
description: Load, index, retrieve, rerank, ground, and cite external knowledge.
---

# Knowledge and RAG

The knowledge module turns documents into scoped chunks and retrieves relevant evidence for a model request or tool call.

## Core pipeline

1. Load documents from files, directories, structured input, source code, URLs, or Tika-supported formats.
2. Split content into chunks while preserving source metadata.
3. Generate embeddings through `embedding_model`.
4. Store documents and index chunks.
5. Retrieve candidates with access scope.
6. Optionally rewrite, rerank, filter, ground, and format results.
7. Inject a cited context block or expose `search_knowledge` as a tool.

## Minimal retriever

```cpp
namespace knowledge = wuwe::agent::knowledge;

auto retriever = std::make_shared<knowledge::knowledge_retriever>(
  std::make_shared<knowledge::file_knowledge_store>("knowledge.jsonl"),
  std::make_shared<knowledge::file_knowledge_index>("index.jsonl"),
  embedding_model,
  knowledge::knowledge_splitter({
    .max_chars = 800,
    .overlap_chars = 80,
  }));

auto loader = knowledge::knowledge_document_loader::make_default();
for (auto& document : loader.load("docs", {
       .metadata = { { "visibility", "public" } },
     })) {
  retriever->ingest(std::move(document));
}

knowledge::knowledge_context context(retriever);
auto augmented = context.augment(std::move(request), "retrieval query");
```

The example assumes an application-provided `embedding_model`. `openai_embedding_model` is available for OpenAI-compatible embedding APIs.

## Storage and indexes

| Layer | Implementations |
| --- | --- |
| Document store | In-memory, file |
| Local index | In-memory, file, SQLite |
| Vector service | Qdrant |
| Generic remote adapters | pgvector, OpenSearch, Milvus HTTP adapters |

The generic remote adapters depend on a compatible HTTP endpoint contract; they are not native database drivers. Validate the target service schema and authentication in the host deployment.

The SQLite knowledge index is durable but performs embedding similarity through a C++ linear scan. It is not an ANN index.

## Retrieval quality

The retriever supports:

- lexical and embedding candidates;
- access-scope filtering;
- static, callback, HTTP, and LLM query rewriting;
- score, BM25, MMR, and cross-encoder rerankers;
- result processing and minimum-score policies;
- retrieval caching;
- trace timing and event sinks;
- evaluation and benchmark helpers.

Keep embedding provider, model, version, dimension, and index schema consistent through `knowledge_indexing_policy`. Rebuild when those values change.

## Loading and bundled parsing

`knowledge_document_loader::make_default()` registers the local file parser, URL loader, and a Tika parser when the bundled runtime can be started. Release packages include Apache Tika and a platform-specific Temurin 21 JRE.

The loader owns the started runtime for its lifetime. Plain text and supported local formats remain available through the parser registry; PDF and Office parsing use Tika.

## RAG service and tools

`knowledge_rag_service` combines upload, retrieval, context construction, citations, and answer generation. `knowledge_grounding_checker` is available as a separate validation step. `knowledge_tool_provider` exposes model-visible search, and knowledge tools can also be registered directly with an MCP server.

High-level RAG context and knowledge-tool policies default to
`knowledge_acl_mode::deny_if_unlabeled`. Ingested documents must therefore carry an
explicit access label such as `visibility=public`, `tenant_id`, `user_id`,
`allowed_users`, or `allowed_roles`. Empty labels are not accepted. The lower-level
`knowledge_access_scope` default remains `permissive` for source compatibility;
security-sensitive applications should use a high-level policy or set the mode
explicitly.

Construct `knowledge_tool_provider` with an `agent_execution_context` or a
host-supplied access scope. Tenant/user/role fields in the legacy model schema are
ignored by default so the model cannot choose its own identity. Set
`allow_model_supplied_access=true` only for a deliberately non-authoritative search
experience.

An empty execution identity preserves the policy's configured tenant/user access.
Once either runtime tenant or user is supplied, both subject fields are projected
from that execution context; Wuwe never combines a configured tenant with a user
from another request. Configure rerankers, query rewriters, caches, and access
filters before concurrent retrieval begins; those setters are configuration APIs,
not live reconfiguration synchronization points.

Retrieved chunks are provenance-labeled and injected as escaped, non-system data
after leading system instructions. As with Memory, promoting untrusted content to
a system message requires the explicit `allow_untrusted_system_message` override.

Grounding reports and citations help a host explain supporting evidence, but they do not guarantee factual correctness. The application should decide how to handle low scores, missing evidence, conflicting sources, and access-denied results.

Bulk ingestion and rebuild operations are available through cancellable `knowledge_task` operations. The asynchronous APIs require the retriever itself to be owned by `std::shared_ptr`; each task retains that ownership until its detached worker exits, preventing use-after-free when the caller releases its reference before `get()` completes. Calling an asynchronous operation on a stack-owned or otherwise unshared retriever is rejected at the API boundary. Retry backoff must be non-negative, is interruptible by `request_cancel()`, and uses an overflow-safe retry loop. Progress callback exceptions are recorded in task errors without breaking the result future. Detached worker boundaries contain both standard and non-standard provider exceptions, so an extension cannot terminate the host process by throwing through a worker entry point.

Batch ingest and detailed rebuild preserve partial-failure semantics for both
standard and non-standard adapter exceptions: one failed document or chunk is
reported without silently abandoning the remaining batch.

See `examples/src/knowledge_retrieval_example.cpp`, `url_rag_example.cpp`, and `knowledge_mcp_example.cpp`.
