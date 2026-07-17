// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sensor/SensorDef.h" // SensorType (for the visual-ID gate)
#include "world/FactionDef.h" // FactionRelation, areFactionsHostile

#include <cstdint>

namespace fl::sensor {

// What an observer BELIEVES a contact is, in the friend/foe/unknown sense (#527). This is NOT the
// target's faction handed over for free — that would be an identification wallhack, the exact thing
// real IFF exists to prevent. It is the honest result of interrogation and identification:
//
//   * Friend  — the contact answered an IFF interrogation with a friendly code. Friendlies squawk, so
//               a friend is known the moment it is detected, at any range. This is the safe direction:
//               the engine never mislabels a friend as anything else.
//   * Foe     — the contact is hostile AND positively identified: you have eyes on it (a visual
//               contact) or you have committed a firing-quality (STT) lock and worked the track. A
//               bare radar blip on a hostile at range is NOT a foe — it is unknown.
//   * Unknown — detected, but not a friend and not a positively-identified foe. A neutral, or a
//               hostile you have not yet identified. The ROE-critical state: you do not shoot unknowns.
//
// So a distant enemy shows as Unknown until you merge (VID) or lock it up — which is exactly the
// commit loop BVR combat turns on, and why a careless missile at an unknown can be a friendly-fire
// kill. Friendlies are always Friend, so team play never depends on the pilot's judgement to avoid
// shooting their own.
enum class Identification : uint8_t { Unknown = 0, Friend = 1, Foe = 2 };

// Gate a byte (wire / Lua) before casting it to Identification.
[[nodiscard]] inline bool isIdentificationOrdinal(uint8_t v) noexcept {
    return v <= static_cast<uint8_t>(Identification::Foe);
}

// The pure IFF classification. `rel` is the observer→target coalition relationship (Friendly / Neutral
// / Hostile); `sensorTypeMask` is the contact's Contact::sensorTypeMask; `firingQuality` is its STT
// hard-lock flag. Depends on nothing but its arguments, so it is trivially testable and identical
// wherever it is called.
[[nodiscard]] inline Identification classifyIff(FactionRelation rel, uint8_t sensorTypeMask,
                                                bool firingQuality) noexcept {
    if (rel == FactionRelation::Friendly)
        return Identification::Friend; // it squawked friendly
    if (rel == FactionRelation::Hostile) {
        // Positively identified = eyes on it, or a firing-quality lock you have committed to.
        const bool visualId = (sensorTypeMask & static_cast<uint8_t>(1u << static_cast<int>(SensorType::Visual))) != 0;
        const bool identified = visualId || firingQuality;
        return identified ? Identification::Foe : Identification::Unknown;
    }
    return Identification::Unknown; // Neutral, or a not-yet-identified hostile
}

// The affiliation-rule relationship used when no coalition registry is threaded through — the same
// fallback semantics as fl::hostile()/areFactionsHostile (FactionDef.h): faction 0 is neutral and has
// no enemies; two distinct non-zero factions are hostile; the same non-zero faction is friendly to
// itself. Keeps the IFF path behaving exactly as the AI's hostility path does before a mission loads.
[[nodiscard]] inline FactionRelation affiliationRelation(uint16_t a, uint16_t b) noexcept {
    if (a == 0 || b == 0)
        return FactionRelation::Neutral;
    if (a == b)
        return FactionRelation::Friendly;
    return FactionRelation::Hostile;
}

} // namespace fl::sensor
