# Fighters Legacy — Claude Code Instructions

## Project Overview

GPL v3 clean-room reimplementation of Jane's Fighters Anthology (1998).
Cross-platform: Windows 10/11, Linux, macOS. Phase 1 (engine foundation) is active.

## Architecture

```
engine/     — core: content system, asset manager, IContentPack interface
platform/   — HAL: Vulkan, SDL3, OpenAL Soft, ENet backends
tools/      — developer utilities
tests/      — Catch2 unit tests
```

The engine is fully content-agnostic. It knows nothing about FA or any specific game.
FA support lives in jomkz/fa-content. No FA-specific code belongs in this repo.

## Build

```bash
# Linux / macOS
cmake --preset debug && cmake --build --preset debug

# Windows (PowerShell)
cmake --preset debug-msvc && cmake --build --preset debug-msvc

# Run tests
ctest --preset debug --output-on-failure
```

See docs/development.md for prerequisites (Vulkan SDK, SDL3, OpenAL, ENet, Catch2).

## Conventions

- Conventional Commits — scopes: engine / renderer / audio / network / content / flight / ai / mission / build / ci / docs
- DCO sign-off required: `git commit -s`
<!-- REUSE-IgnoreStart -->
- SPDX header required on all new .cpp/.h files: `// SPDX-License-Identifier: GPL-3.0-or-later`
<!-- REUSE-IgnoreEnd -->
- All code must compile on Windows (MSVC 2022), Linux (GCC/Clang), macOS (Apple Clang)
- `CMAKE_COMPILE_WARNING_AS_ERROR=ON` in debug builds — fix all warnings

## Key Files

- `README.md` — master plan (authoritative design doc)
- `docs/architecture.md` — engine architecture overview
- `docs/development.md` — build prerequisites per platform
- `GOVERNANCE.md` — decision-making and RFC process
- `CMakePresets.json` — all build presets (debug / release / coverage / asan / msvc variants)
