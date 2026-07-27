// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

namespace fl {

// Per-faction readiness posture. Server-authoritative; a mission's `sides[].alert` sets the starting
// level and Lua/AI scripts change it at runtime via world.set_alert_level(). Ordinals are a stable
// WIRE contract -- they are the payload of MsgAlertLevelChange (0x26, #162) and of the IWorldAiProvider
// WorldEvolutionDelta (#163), so renumbering them changes what a posture means to every client.
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

// Authoring/scripting vocabulary for both enums. One table per enum, shared by the escalation-policy
// TOML parser, the mission-YAML `alert:` field, the Lua world.* bindings and validate-mission -- so a
// renamed level cannot mean one thing to the parser and another to a script.
[[nodiscard]] inline std::string_view alertLevelName(AlertLevel level) noexcept {
    switch (level) {
    case AlertLevel::Peacetime:
        return "peacetime";
    case AlertLevel::Elevated:
        return "elevated";
    case AlertLevel::Conflict:
        return "conflict";
    case AlertLevel::WarState:
        return "war_state";
    }
    return "peacetime";
}

[[nodiscard]] inline bool alertLevelFromString(std::string_view s, AlertLevel& out) noexcept {
    if (s == "peacetime") {
        out = AlertLevel::Peacetime;
        return true;
    }
    if (s == "elevated") {
        out = AlertLevel::Elevated;
        return true;
    }
    if (s == "conflict") {
        out = AlertLevel::Conflict;
        return true;
    }
    if (s == "war_state") {
        out = AlertLevel::WarState;
        return true;
    }
    return false;
}

[[nodiscard]] inline std::string_view escalationStageName(EscalationStage stage) noexcept {
    switch (stage) {
    case EscalationStage::Clean:
        return "clean";
    case EscalationStage::InZone:
        return "in_zone";
    case EscalationStage::Warned:
        return "warned";
    case EscalationStage::Intercept:
        return "intercept";
    case EscalationStage::Hostile:
        return "hostile";
    }
    return "clean";
}

// Gate an attacker-supplied or script-supplied byte before casting it to the enum (the
// isWingmanCommandOrdinal / isPeerRoleOrdinal rule -- a wire or Lua value is never trusted).
[[nodiscard]] inline bool isAlertLevelOrdinal(uint8_t v) noexcept {
    return v <= static_cast<uint8_t>(AlertLevel::WarState);
}

[[nodiscard]] inline bool isEscalationStageOrdinal(uint8_t v) noexcept {
    return v <= static_cast<uint8_t>(EscalationStage::Hostile);
}

} // namespace fl
