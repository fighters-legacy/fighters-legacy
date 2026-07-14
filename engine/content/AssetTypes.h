// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fl {

enum class AssetType : uint8_t {
    Mesh,
    Texture,
    Audio,
    FlightModel,
    Mission,
    Terrain,
    AIScript,
    EntityDef,
    SensorDef,
    Weapon,
    Manual, // hand-written aircraft prose (#821); the NUMBERS are generated, never authored
    Count
};

// Raw-byte asset base. Format-specific fields are added by the renderer/audio
// workstreams once those subsystems exist. All IContentPack load methods return subtypes.
struct AssetBase {
    std::string name;           // canonical asset name, lowercase
    std::vector<uint8_t> bytes; // raw file contents
};

// Distinct types preserve type-safe function signatures while sharing the same layout.
struct MeshData : AssetBase {};
struct TextureData : AssetBase {};
struct AudioBuffer : AssetBase {};
struct FlightModel : AssetBase {};
struct MissionData : AssetBase {};
struct TerrainData : AssetBase {};
struct AIScript : AssetBase {};
struct EntityDefData : AssetBase {};
struct SensorDefData : AssetBase {};
struct WeaponDefData : AssetBase {};
struct ManualProse : AssetBase {};

} // namespace fl
