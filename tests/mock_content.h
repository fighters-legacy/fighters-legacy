// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared IContentPack test double. Kept out of mock_hal.h so HAL-only tests don't pull in
// engine/content (IContentPack.h drags in content/AssetTypes.h). Naming convention (mirrors
// mock_hal.h): Null* = no-op base. Derive and override only the loaders a test exercises
// (e.g. loadAudio for an audio fixture, resolveTilePath for a terrain fixture).

#include "content/IContentPack.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fl {

// Null-object content pack: every query empty / nullopt / false, init Ready.
struct NullContentPack : IContentPack {
    const char* name() const override {
        return "null";
    }
    const char* version() const override {
        return "0";
    }
    const char* id() const override {
        return "null";
    }
    const char* namespaceId() const override {
        return "null";
    }
    int priority() const override {
        return 0;
    }
    const char* rootDirectory() const override {
        return nullptr;
    }
    Status init() override {
        return Status::Ready;
    }
    bool configure(IWindow*) override {
        return true;
    }
    bool hasAsset(const char*, AssetType) const override {
        return false;
    }
    std::optional<MeshData> loadMesh(const char*) override {
        return std::nullopt;
    }
    std::optional<TextureData> loadTexture(const char*) override {
        return std::nullopt;
    }
    std::optional<AudioBuffer> loadAudio(const char*) override {
        return std::nullopt;
    }
    std::optional<FlightModel> loadFlightModel(const char*) override {
        return std::nullopt;
    }
    std::optional<MissionData> loadMission(const char*) override {
        return std::nullopt;
    }
    std::optional<TerrainData> loadTerrain(const char*) override {
        return std::nullopt;
    }
    std::optional<AIScript> loadAIScript(const char*) override {
        return std::nullopt;
    }
    std::optional<EntityDefData> loadEntityDef(const char*) override {
        return std::nullopt;
    }
    std::optional<SensorDefData> loadSensorDef(const char*) override {
        return std::nullopt;
    }
    std::optional<WeaponDefData> loadWeaponDef(const char*) override {
        return std::nullopt;
    }
    std::optional<ManualProse> loadManualProse(const char*) override {
        return std::nullopt;
    }
    std::vector<std::string> listAssets(AssetType) const override {
        return {};
    }
    std::optional<std::string> loadConfig(const char*) const override {
        return std::nullopt;
    }
    std::optional<std::string> resolveTilePath(const char*, uint8_t, uint8_t, uint32_t, uint32_t,
                                               TileLayer) const override {
        return std::nullopt;
    }
    TrustLevel getTrustLevel() const override {
        return TrustLevel::Unsigned;
    }
    bool isNativePlugin() const override {
        return false;
    }
};

} // namespace fl
