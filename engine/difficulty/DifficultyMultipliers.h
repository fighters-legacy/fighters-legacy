// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config/DifficultySettings.h"

#include <string_view>

namespace fl {

class AssetManager;
class IFilesystem;
class ILogger;

// Preset-owned fields for one named preset. Excludes invulnerability and
// unlimitedWeapons (player-only accessibility options, not preset-driven).
struct PresetValues {
    FlightAssists flightAssists;
    bool aimAssist;
    EnemyLabels enemyLabels;
    RadarRealism radarRealism;
    bool blackoutRedout;
    bool fuelConsumption;
    RefuelingMode inFlightRefueling;
    bool friendlyFire;
    bool crashDamage;
    RearmMode rearmMode;
    float reactionTimeS;
    float aimErrorDeg;
    float radarSensorRange;
    CountermeasureUse countermeasureUse;
    EnergyManagement energyManagement;
    float samEngagementRange;
    SamRadarShutdown samRadarShutdown;
};

// Loaded from data/difficulty.toml. Provides per-preset defaults used when the
// player selects a named difficulty. Mods override by shipping their own
// data/difficulty.toml at higher AssetManager priority.
class DifficultyMultipliers {
  public:
    // Returns compile-time hardcoded defaults matching the issue spec table.
    // Used as fallback when no file can be loaded, and in tests.
    static DifficultyMultipliers defaults();

    // Reads "data/difficulty.toml" directly from PathDomain::Assets via fs.
    // Missing file → no Warn, returns defaults().
    // Parse failure or unknown values → Warn per-field, falls back to hardcoded value.
    static DifficultyMultipliers load(IFilesystem& fs, ILogger& logger);

    // Tries am.loadConfig("difficulty.toml") first (highest-priority mod wins).
    // Falls back to load(fs, logger) if no pack provides the file.
    static DifficultyMultipliers load(AssetManager& am, IFilesystem& fs, ILogger& logger);

    // p must be Cadet, Pilot, or Ace — not Custom.
    const PresetValues& preset(DifficultyPreset p) const;

    // Applies the preset-owned fields of p to ds and stamps ds.preset = p.
    // Preserves ds.toggles.invulnerability and ds.toggles.unlimitedWeapons.
    // p must be Cadet, Pilot, or Ace — not Custom.
    void applyPreset(DifficultyPreset p, DifficultySettings& ds) const;

  private:
    DifficultyMultipliers() = default;
    static DifficultyMultipliers parseFrom(std::string_view text, ILogger& logger);

    PresetValues m_cadet;
    PresetValues m_pilot;
    PresetValues m_ace;
};

} // namespace fl
