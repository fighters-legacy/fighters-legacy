// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

// Per-faction readiness posture. Server-authoritative; missions set the starting
// level and Lua/AI scripts change it at runtime. Ordinals are a stable contract:
// they feed the (deferred) FL_MSG_ALERT_LEVEL_CHANGE wire broadcast (#162) and the
// IWorldAiProvider WorldEvolutionDelta (#163).
enum class AlertLevel : uint8_t {
    Peacetime = 0, // normal ops; slow/no zone response
    Elevated = 1,  // increased readiness; faster intercept
    Conflict = 2,  // active hostilities; minimal warning window
    WarState = 3,  // weapons free on detection; no warning
};

// Per-intruder escalation progress within an airspace zone (consumed by AlertSystem, #162).
enum class EscalationStage : uint8_t {
    Clean = 0,     // not in zone (or cooldown complete)
    InZone = 1,    // entered zone; warning timer counting
    Warned = 2,    // radio warning issued; intercept timer counting
    Intercept = 3, // interceptors scrambled; hostile timer counting
    Hostile = 4,   // weapons free for zone owner against this entity
};

} // namespace fl
