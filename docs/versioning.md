---
id: versioning
title: Versioning and compatibility
sidebar_position: 3
description: Understand Wuwe 1.x source, binary, protocol, and storage compatibility.
---

# Versioning and compatibility

Wuwe 1.0.0 defines the first stable public API. Starting with 1.0.0, Wuwe uses
semantic versioning for its documented public C++ source API.

## Public compatibility surface

The 1.x source-compatibility promise covers:

- installed headers under `include/wuwe`, except paths or namespaces named
  `detail`;
- documented public classes, functions, enums, structures, and constants;
- the `wuwe::wuwe` CMake target and documented CMake options;
- serialized contracts that publish their own schema or protocol version;
- documented tool names and JSON schemas.

Within 1.x, compatible additions may be made in minor releases. Removing or
incompatibly changing the documented public source API requires a new major
version. Deprecated public APIs remain available through 1.x unless retaining
them would create a material security or correctness problem; any exception is
called out in the release notes with a migration path.

Private implementation files, `detail` namespaces, undocumented behavior,
test helpers, examples, and external-service behavior are not stable API.

## Source compatibility versus ABI

Wuwe distributes a native C++ SDK and does not promise that a binary compiled
against one Wuwe release can be relinked or run with another release without
recompilation. Compiler, standard library, build options, public dependencies,
structure layout, templates, and virtual interfaces can all affect the ABI.

The CMake package uses `SameMajorVersion` compatibility. This means a consumer
requesting Wuwe 1 can configure against a later 1.x SDK and rebuild from source;
it does not declare drop-in binary compatibility. Never mix headers and
libraries from different Wuwe releases.

## Protocol and schema compatibility

MCP, A2A, Agent Host, observability events, durable run records, and SQLite
stores use explicit protocol or schema versions where compatibility matters.
Readers reject unknown incompatible versions instead of silently guessing.
Breaking wire-format or storage changes require negotiation, a new schema
version with migration, or a new major Wuwe release when the public contract is
affected.

## Support matrix

The platform and dependency combinations certified for a release are listed in
the README and package manifest. Portability outside that matrix is an
engineering goal, not a release guarantee. Optional backends only provide the
enforcement described by their capability contract.

## Consumer upgrade rule

For every SDK upgrade:

1. use matching headers, libraries, CMake files, and runtime sidecars;
2. configure and rebuild the complete consumer;
3. review the changelog and migration notes;
4. run product-level integration, trajectory, security, and persistence tests.
