// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "world/AlertLevel.h"

#include <array>
#include <cstddef>
#include <string>

namespace fl {

// Number of AlertLevel enumerators; an EscalationPolicy carries one dwell row per level.
inline constexpr std::size_t kAlertLevelCount = 4;

// Dwell thresholds for one alert level, in seconds measured from the moment an intruder enters the
// zone (cumulative, NOT per-stage): at Peacetime 45/90/180 means warned at 45 s, intercepted at
// 90 s, weapons-free at 180 s. Thresholds must be non-decreasing; the parser enforces it.
//
// A threshold of 0 means "this stage is already satisfied on entry", so an all-zero row is the
// war-state posture: an intruder is hostile the instant it crosses the boundary, with no warning.
struct EscalationDwell {
    double warningDwellS{0};
    double interceptDwellS{0};
    double hostileDwellS{0};

    // Whether leaving the zone unwinds the escalation. With complianceReset the intruder is
    // forgotten complianceCooldownS seconds after it departs (turning back works); without it the
    // stage sticks for the session, which is the point of a conflict/war posture.
    bool complianceReset{true};
    double complianceCooldownS{0};
};

// A named escalation posture, authored as a content-pack TOML (zones/policies/<id>.toml) and
// referenced by an airspace zone's `policy:` field. One dwell row per AlertLevel: the zone owner's
// live alert level selects the row every tick, so raising a faction's alert level tightens every
// zone it owns at once without touching the zones themselves.
struct EscalationPolicy {
    std::string id;
    std::string name;
    std::array<EscalationDwell, kAlertLevelCount> byLevel{};

    [[nodiscard]] const EscalationDwell& forLevel(AlertLevel level) const noexcept {
        const auto idx = static_cast<std::size_t>(level);
        return byLevel[idx < kAlertLevelCount ? idx : 0];
    }
};

// The posture used when a zone names no policy, or names one the content pack does not ship: a
// peacetime-ish standing warning that escalates slowly and forgives compliance. A missing policy
// must not silently make a zone inert (nobody would notice) nor instantly hostile (a mission would
// become unplayable from a typo), so the fallback is the mildest posture that still enforces.
[[nodiscard]] inline EscalationPolicy defaultEscalationPolicy() {
    EscalationPolicy p;
    p.id = "builtin:default";
    p.name = "Default Intercept";
    p.byLevel[static_cast<std::size_t>(AlertLevel::Peacetime)] = {45, 90, 180, true, 300};
    p.byLevel[static_cast<std::size_t>(AlertLevel::Elevated)] = {20, 45, 90, true, 180};
    p.byLevel[static_cast<std::size_t>(AlertLevel::Conflict)] = {5, 15, 60, false, 0};
    p.byLevel[static_cast<std::size_t>(AlertLevel::WarState)] = {0, 0, 0, false, 0};
    return p;
}

} // namespace fl
