// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "world/AlertLevel.h"

#include <cstdint>
#include <string>

namespace fl {

// Relationship between two factions. Ordinals are a stable contract (Lua bindings, #413).
enum class FactionRelation : uint8_t { Friendly = 0, Neutral = 1, Hostile = 2 };

// Static faction definition. TOML parsing (factions/*.toml) is deferred to #162; this
// is the in-memory descriptor loaded into FactionRegistry before the sim starts.
struct FactionDef {
    std::string id;
    std::string name;
    AlertLevel startingAlertLevel{AlertLevel::Peacetime};
};

} // namespace fl
