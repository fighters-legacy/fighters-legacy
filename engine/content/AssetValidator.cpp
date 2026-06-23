// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/AssetValidator.h"

#include <cstring>

namespace fl {

AssetValidator::AssetValidator(ValidationLimits limits) : m_limits(limits) {}

// Magic byte sequences for supported formats.
// clang-format off
static const uint8_t kMagicGltfBinary[] = {0x67, 0x6C, 0x54, 0x46}; // "glTF"
static const uint8_t kMagicKtx2[]       = {                          // «KTX 20»\r\n\x1a\n
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
static const uint8_t kMagicPng[]        = {0x89, 0x50, 0x4E, 0x47}; // \x89PNG
static const uint8_t kMagicOgg[]        = {0x4F, 0x67, 0x67, 0x53}; // OggS
// clang-format on

static bool startsWith(std::span<const uint8_t> header, const uint8_t* magic, std::size_t len) {
    if (header.size() < len)
        return false;
    return std::memcmp(header.data(), magic, len) == 0;
}

ValidationResult AssetValidator::validate(AssetType type, std::span<const uint8_t> header,
                                          std::size_t totalSize) const {
    if (header.empty())
        return {false, "empty asset data"};

    switch (type) {
    case AssetType::Mesh:
        // Accept binary glTF ("glTF") or JSON glTF (starts with '{')
        if (!startsWith(header, kMagicGltfBinary, sizeof(kMagicGltfBinary)) && header[0] != '{')
            return {false, "mesh: invalid magic bytes (expected glTF binary or JSON)"};
        if (totalSize > m_limits.maxMesh)
            return {false, "mesh: exceeds size limit"};
        break;

    case AssetType::Texture:
        // Accept KTX2 or PNG
        if (!startsWith(header, kMagicKtx2, sizeof(kMagicKtx2)) && !startsWith(header, kMagicPng, sizeof(kMagicPng)))
            return {false, "texture: invalid magic bytes (expected KTX2 or PNG)"};
        if (totalSize > m_limits.maxTexture)
            return {false, "texture: exceeds size limit"};
        break;

    case AssetType::Audio:
        if (!startsWith(header, kMagicOgg, sizeof(kMagicOgg)))
            return {false, "audio: invalid magic bytes (expected OggS)"};
        if (totalSize > m_limits.maxAudio)
            return {false, "audio: exceeds size limit"};
        break;

    case AssetType::AIScript:
        // Reject precompiled Lua bytecode (\x1b magic byte)
        if (header[0] == 0x1B)
            return {false, "script: precompiled Lua bytecode is not permitted"};
        if (totalSize > m_limits.maxLua)
            return {false, "script: exceeds size limit"};
        break;

    case AssetType::FlightModel:
    case AssetType::EntityDef:
        // TOML — plain text, no magic bytes; size check only
        if (totalSize > m_limits.maxToml)
            return {false, "config: exceeds size limit"};
        break;

    case AssetType::Mission:
        // YAML — plain text, no magic bytes; size check only
        if (totalSize > m_limits.maxYaml)
            return {false, "mission: exceeds size limit"};
        break;

    case AssetType::Terrain:
        if (totalSize > m_limits.maxTerrainChunk)
            return {false, "terrain: exceeds size limit"};
        break;

    default:
        break;
    }

    return {true, {}};
}

} // namespace fl
