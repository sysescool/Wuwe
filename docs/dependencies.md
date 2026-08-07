---
id: dependencies
title: Dependencies
sidebar_position: 3
description: Build, link, and runtime dependencies for Wuwe 1.0.0.
---

# Dependencies

Wuwe separates C++ build dependencies from runtime sidecars and external services. This matters because the release contains a static SDK: consuming applications must still make the enabled public link dependencies available to CMake.

## Official release profiles

| Capability | Windows x64 | Linux x64 | macOS 14+ arm64 |
| --- | --- | --- | --- |
| Default HTTP backend | cpr/libcurl | cpr/libcurl | cpr/libcurl |
| TLS | Schannel | OpenSSL | OpenSSL |
| SQLite | Required | Required | Required |
| Document parsing | Tika and bundled Temurin 21 | Tika and bundled Temurin 21 | Tika and bundled Temurin 21 |

The configure step uses `vcpkg.json` and its pinned `builtin-baseline` to restore missing manifest dependencies into the selected build tree. It does not install them globally.

## Inventory

| Dependency | Used for | Delivery |
| --- | --- | --- |
| CMake and a C++20 compiler | Building Wuwe and consumers | Build environment |
| cpr/libcurl | Default HTTP transport | Fetched at the revision pinned in `src/CMakeLists.txt` for official builds; source consumers may provide an existing `cpr::cpr` target |
| cpp-httplib | Alternate HTTP client and MCP HTTP listener | Checked-in header |
| OpenSSL | Linux/macOS TLS and the optional Windows OpenSSL profile | vcpkg or another compatible development package |
| Python 3 | Controlled Python execution and the macOS restricted-process backend | Host installation; configure `python_interpreter` explicitly when it is not on `PATH` |
| SQLite3 | Durable memory and knowledge storage | vcpkg in official profiles; compatible package for SDK consumers |
| nlohmann/json | JSON representation and codecs | Checked-in headers |
| Apache Tika Server | PDF and Office parsing | Default bundled sidecar; may be omitted or externally managed |
| Temurin 21 JRE | Runs the bundled Tika server | Default platform bundle; may be omitted when Java is supplied by the host |
| Qdrant | Optional vector service | Deployed separately and accessed over HTTP |

## TLS selection

Set `WUWE_TLS_BACKEND` at configure time:

| Value | Behavior |
| --- | --- |
| `native` | Uses the platform/libcurl-native backend; Windows selects Schannel |
| `openssl` | Requires OpenSSL and links it explicitly |
| `auto` | Uses OpenSSL when found, otherwise the native backend |

The official presets use deterministic choices: `native` on Windows and `openssl` on Linux/macOS. The Windows OpenSSL variant is available as `windows-vcpkg-openssl`.

```powershell
cmake --preset windows-vcpkg-openssl
cmake --build --preset windows-vcpkg-openssl-release
```

`WUWE_ENABLE_HTTPLIB_SSL` controls HTTPS in the cpp-httplib backend. It is effective only when the selected build has OpenSSL.

## SQLite selection

Set `WUWE_SQLITE_MODE` at configure time:

| Value | Behavior |
| --- | --- |
| `on` | Requires SQLite; configuration fails when it is unavailable |
| `auto` | Enables SQLite when found |
| `off` | Builds without SQLite-backed capabilities |

The official presets use `on`. A SQLite-enabled installed package calls `find_dependency` for the provider selected during the Wuwe build.

SQLite provides durable local storage, not a vector database. `sqlite_knowledge_index` stores embeddings as JSON and computes similarity with a linear scan in C++. Use Qdrant or an application-specific remote index for large semantic collections or service-scale concurrency.

## Installed-package behavior

```cmake
find_package(wuwe 1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE wuwe::wuwe)
```

The exported configuration exposes the resolved HTTP, TLS, OpenSSL, cpp-httplib HTTPS, and SQLite capabilities. Missing required link dependencies fail during `find_package(wuwe)` instead of appearing later as unresolved targets.

The bundled curl build disables Brotli and zstd to keep the static link interface reproducible. gzip and deflate remain available through curl's zlib support.

Dependency-specific CPR and curl options are scoped to the dependency fetch.
Wuwe no longer forces global `BUILD_SHARED_LIBS`, TLS, or compression cache
values in a parent CMake project.
