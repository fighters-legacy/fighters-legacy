// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The persistent pilot logbook (#674): the career record that gives a debrief meaning — kill tallies
// by target class, per-weapon-category accuracy, and career counters (missions, ejections, landing
// quality). The debrief (#634) writes deltas into it; rank/medal evaluation and the campaign
// (#584/#635) read it. Kill/score events (#626) are the single source of truth.
//
// One taxonomy, many consumers: kills are tallied by the ENTITY taxonomy (ObjectCategory ordinal), not
// a parallel list — the array is indexed by `static_cast<int>(ObjectCategory)`. This header stays free
// of engine-entity so engine-config keeps its low layer; the caller maps a killed entity's category to
// an ordinal (guarded by kKillClassCount).

#include <cstdint>

namespace fl {

// Broad weapon-employment classes tracked for accuracy (mirrors the FA pilot save's A/A gun, A/A
// missile, ground-attack, naval split). The kill feed's weaponClass maps onto these.
enum class WeaponLogClass : uint8_t { AirGun = 0, AirMissile = 1, GroundAttack = 2, Naval = 3, Count = 4 };

struct WeaponAccuracy {
    uint32_t shots{0};
    uint32_t hits{0};
    uint32_t kills{0};
    // Hit fraction in [0,1]; 0 when nothing was fired.
    [[nodiscard]] float hitRate() const noexcept {
        return shots == 0 ? 0.f : static_cast<float>(static_cast<double>(hits) / shots);
    }
};

struct PilotLogbook {
    // Kill tallies by target class, indexed by ObjectCategory ordinal (air/ground/naval/…). 8 slots
    // give headroom over the current 7 ObjectCategory values so a new category never overflows.
    static constexpr int kKillClassCount = 8;
    uint32_t killsByClass[kKillClassCount]{};

    WeaponAccuracy weapons[static_cast<int>(WeaponLogClass::Count)]{};

    // Career counters.
    uint32_t missionsFlown{0};
    uint32_t missionsFailed{0};
    uint32_t ejections{0};
    float bestLandingScore{0.f};
    float lastLandingScore{0.f};

    [[nodiscard]] uint32_t totalKills() const noexcept {
        uint32_t n = 0;
        for (uint32_t k : killsByClass)
            n += k;
        return n;
    }

    // Record one air-to-air / air-to-ground / naval kill against a target of the given class ordinal
    // (ObjectCategory). Out-of-range ordinals are ignored (never overflow the array).
    void recordKill(int classOrdinal) noexcept {
        if (classOrdinal >= 0 && classOrdinal < kKillClassCount)
            ++killsByClass[classOrdinal];
    }
    void recordShot(WeaponLogClass w, uint32_t n = 1) noexcept {
        weapons[static_cast<int>(w)].shots += n;
    }
    void recordHit(WeaponLogClass w, uint32_t n = 1) noexcept {
        weapons[static_cast<int>(w)].hits += n;
    }
    void recordWeaponKill(WeaponLogClass w) noexcept {
        ++weapons[static_cast<int>(w)].kills;
    }
    void recordMission(bool success) noexcept {
        ++missionsFlown;
        if (!success)
            ++missionsFailed;
    }
    void recordEjection() noexcept {
        ++ejections;
    }
    void recordLanding(float score) noexcept {
        lastLandingScore = score;
        if (score > bestLandingScore)
            bestLandingScore = score;
    }
};

} // namespace fl
