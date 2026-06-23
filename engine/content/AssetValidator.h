// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "content/AssetTypes.h"
#include <cstddef>
#include <span>
#include <string>

namespace fl {

// Size limits (bytes) enforced by AssetValidator::validate(). All fields have
// sensible defaults; pass a custom ValidationLimits to AssetManager to override.
struct ValidationLimits {
    std::size_t maxMesh = 50ULL * 1024 * 1024;         // 50 MB  — glTF meshes
    std::size_t maxTexture = 128ULL * 1024 * 1024;     // 128 MB — KTX2/PNG textures
    std::size_t maxAudio = 256ULL * 1024 * 1024;       // 256 MB — OGG audio
    std::size_t maxToml = 1ULL * 1024 * 1024;          // 1 MB   — TOML config / entity def / flight model
    std::size_t maxYaml = 4ULL * 1024 * 1024;          // 4 MB   — YAML missions / campaigns
    std::size_t maxLua = 512ULL * 1024;                // 512 KB — Lua AI / mission scripts
    std::size_t maxTerrainChunk = 16ULL * 1024 * 1024; // 16 MB  — terrain JSON + PNG chunk
};

struct ValidationResult {
    bool valid = true;
    std::string reason; // non-empty when valid == false
};

// Validates asset bytes via magic-byte checks and size limits. Called from
// AssetManager before caching any asset returned by a content pack. Covers
// compiled plugins as well as directory mods because validation happens in
// the manager layer, not in individual pack implementations.
class AssetValidator {
  public:
    explicit AssetValidator(ValidationLimits limits = {});

    // header: first bytes of the asset (up to 16 bytes; may be shorter for tiny files).
    // totalSize: total unread byte count of the asset.
    // Returns {true, ""} on success, {false, reason} on any check failure.
    ValidationResult validate(AssetType type, std::span<const uint8_t> header, std::size_t totalSize) const;

  private:
    ValidationLimits m_limits;
};

} // namespace fl
