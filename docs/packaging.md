---
id: packaging
title: Packaging
sidebar_position: 4
description: Build and inspect the Wuwe SDK archives.
---

# Packaging

Wuwe produces a platform-specific SDK archive containing the library, public headers, CMake package files, examples, documentation, release metadata, and the default document-parsing runtime.

The archive version is read from the repository `VERSION` file. CMake package
metadata, `vcpkg.json`, public `wuwe/version.hpp`, protocol defaults, CI artifact
names, and release documentation use the same `1.0.0` release identity.

## Build an archive

Windows x64:

```powershell
$env:VCPKG_ROOT = "D:\tools\vcpkg"
cmake --preset windows-vcpkg
cmake --build --preset windows-vcpkg-release
ctest --preset windows-vcpkg-release
.\tools\package-wuwe.ps1 -BuildDir build-vcpkg -Configuration Release
```

Linux x64:

```bash
export VCPKG_ROOT="$HOME/vcpkg"
cmake --preset linux-vcpkg
cmake --build --preset linux-vcpkg-release
ctest --preset linux-vcpkg-release
bash ./tools/package-wuwe.sh --build-dir build-linux-vcpkg --configuration Release
```

Windows produces a `.zip`; Linux produces a `.tar.gz`. Build each archive on its target operating system so native binaries, permissions, symbolic links, and the bundled JRE remain correct.

## Archive contents

- public headers and static libraries
- `wuwe-config.cmake` and exported CMake targets
- docs and example source
- the strict local Skill package example under `examples/skills`
- `README.md`, `CHANGELOG.md`, `LICENSE`, `VERSION`, and `vcpkg.json`
- `manifest.json` with resolved build and runtime capabilities
- `checksums.sha256`
- `runtime/tika/tika-server-standard.jar`
- `runtime/jre` with the platform-specific Temurin 21 runtime

The packaging scripts use the pinned assets under `third_party/runtime/` and verify their checksums. They do not download Tika or Java while packaging.

## Install without creating an archive

```powershell
cmake --install build-vcpkg --config Release --prefix install
```

The install tree has the same SDK layout and, by default, the bundled runtime sidecars.

Wuwe 1.x CMake package compatibility is major-version scoped. A consumer asking
for Wuwe 1 can use a later 1.x package after recompilation. This is a source
compatibility promise, not a cross-release C++ ABI promise. See
[Versioning and compatibility](versioning.md).

Tika and the JRE are independently optional:

```powershell
cmake -S . -B build-core -DWUWE_INSTALL_TIKA_RUNTIME=OFF -DWUWE_INSTALL_BUNDLED_JRE=OFF
cmake --install build-core --config Release --prefix install-core
```

For the Windows archive script, `-ExcludeTikaRuntime` and `-ExcludeBundledJre` remove the corresponding runtime. The Linux archive script provides `--without-tika` and `--without-jre`. Both scripts record the decision in `manifest.json`. A package can include Tika without a JRE when the deployment supplies Java through `PATH`, or omit Tika entirely when document parsing is not needed or an external endpoint is managed by the host.

## Default document runtime

```cpp
auto loader =
  wuwe::agent::knowledge::knowledge_document_loader::make_default();
```

The default loader registers local parsers, starts the bundled Tika server when the runtime is discoverable, and keeps that process alive for the loader lifetime. It uses `runtime/jre/bin/java.exe` on Windows and `runtime/jre/bin/java` on Linux.

## What is not bundled

The archive is a complete Wuwe SDK, not a copy of every C++ development package. A consumer of the static library must provide the public dependencies recorded by the build:

- SQLite3 for SQLite-enabled packages;
- OpenSSL for OpenSSL-enabled packages.

Use `manifest.json` and the exported CMake package as the source of truth for a specific archive.
