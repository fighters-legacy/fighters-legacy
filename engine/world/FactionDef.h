// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "world/AlertLevel.h"

#include <cstdint>
#include <string>

namespace fl {

// Relationship between two factions. Ordinals are a stable contract (Lua bindings, #413).
enum class FactionRelation : uint8_t { Friendly = 0, Neutral = 1, Hostile = 2 };

// Default team-affiliation hostility rule: two entities are hostile when both carry a
// non-neutral faction and their factions differ. Faction 0 (neutral / unset) is never
// hostile and has no enemies. This is the affiliation-level classification used by AI
// threat conditions (#465) and sensing (#690); relationship-matrix classification via
// FactionRegistry (allies / coalitions) is the upgrade once the registry is threaded into
// the sensing/AI path (Epic F). Keeping it a free function makes it the single source of
// truth both paths share.
[[nodiscard]] inline bool areFactionsHostile(uint16_t a, uint16_t b) noexcept {
    return a != 0 && b != 0 && a != b;
}

// Static faction definition. There is deliberately no factions/*.toml: a mission's `sides:` block is
// the single source of truth for which coalitions exist and each one's starting posture (#162), so
// this is purely the in-memory descriptor MissionSetup loads into FactionRegistry before the sim
// starts.
struct FactionDef {
    std::string id;
    std::string name;
    AlertLevel startingAlertLevel{AlertLevel::Peacetime};
};

} // namespace fl
