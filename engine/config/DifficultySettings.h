// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

// Custom = player has overridden at least one field from the last applied named preset.
enum class DifficultyPreset : uint8_t { Cadet, Pilot, Ace, Custom };

enum class FlightAssists : uint8_t { AllOn, GLimiterOnly, AllOff };
enum class EnemyLabels : uint8_t { Always, OnLock, Off };
enum class RadarRealism : uint8_t { Simple, Standard, Full };
enum class RefuelingMode : uint8_t { Auto, Simplified, Manual };
enum class RearmMode : uint8_t { Instantaneous, Timed, SupplyLimited };

enum class CountermeasureUse : uint8_t { Never, Reactive, Proactive };
enum class EnergyManagement : uint8_t { Passive, Standard, AggressiveBfm };
enum class SamRadarShutdown : uint8_t { Never, Sometimes, Always };

// The 12 gameplay toggles. Defaults match the Cadet preset.
// invulnerability and unlimitedWeapons are player-only accessibility options;
// they are not preset-driven and do not trigger a Custom stamp.
struct GameplayToggles {
    FlightAssists flightAssists = FlightAssists::AllOn;
    bool aimAssist = true;
    bool invulnerability = false;
    bool unlimitedWeapons = false;
    EnemyLabels enemyLabels = EnemyLabels::Always;
    RadarRealism radarRealism = RadarRealism::Simple;
    bool blackoutRedout = false;
    bool fuelConsumption = false;
    RefuelingMode inFlightRefueling = RefuelingMode::Auto;
    bool friendlyFire = false;
    bool crashDamage = false;
    RearmMode rearmMode = RearmMode::Instantaneous;
};

// The 7 AI scaling parameters. Defaults match the Cadet preset.
// radarSensorRange and samEngagementRange are stored as fractions [0, 1].
struct AiScaling {
    float reactionTimeS = 1.5f;
    float aimErrorDeg = 8.0f;
    float radarSensorRange = 0.50f;
    CountermeasureUse countermeasureUse = CountermeasureUse::Never;
    EnergyManagement energyManagement = EnergyManagement::Passive;
    float samEngagementRange = 0.60f;
    SamRadarShutdown samRadarShutdown = SamRadarShutdown::Never;
};

struct DifficultySettings {
    DifficultyPreset preset = DifficultyPreset::Cadet;
    GameplayToggles toggles = {};
    AiScaling ai = {};
};

} // namespace fl
