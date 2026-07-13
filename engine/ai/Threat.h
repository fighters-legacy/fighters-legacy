// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"

#include <glm/glm.hpp>

// Threat selection helpers for the scripted wingman (#610).
//
// PROVISIONAL, AND DELIBERATELY SO. There is no sensor, radar, IFF or lock system yet — the shared
// sensor core is #677 (SensorDef + Contact tracks, locked by the #678 decision record), detection
// math is #684, and player avionics are #526. Until those land, "the target my lead has designated"
// has to be resolved from something the server already knows, and the honest answer is BORESIGHT:
// the hostile nearest the lead's look axis.
//
// The look axis is NOT newly trusted client data — MsgClientInput has carried `viewAxis` (a
// normalized world-space look direction) since the camera work, and WorldBroadcaster already stores
// it per peer at 60 Hz in PeerInputState::viewAxis. So the wingman designates against state the
// server owns and the sim already consumes, which is exactly the property a targeting system must
// have.
//
// THE SEAM: fl-server passes designateBoresightTarget through a std::function (a TargetDesignator).
// When the sensor framework lands, that lambda is re-pointed at "the Contact track the player has
// locked" and NOTHING else changes — not the grammar, not the wire format, not the client. Look for
// setWingmanOrderHandler's designator in server/fl-server/main.cpp.
//
// Both functions consider only entities HOSTILE to the querying entity, per fl::areFactionsHostile:
// different non-zero factions. A faction-0 (neutral) entity has no enemies at all, which is why
// players must carry a faction for any of this to work ([world] player_faction, default 1).

namespace fl {
class EntityManager;
class SpatialIndex;
struct EntityState;
} // namespace fl

namespace fl::ai {

// Default designation envelope. Generous enough to designate what a pilot would call out, tight
// enough that "attack my target" cannot mean "attack something behind me".
inline constexpr float kDefaultDesignateRangeM = 15000.f;
inline constexpr float kDefaultDesignateHalfAngleDeg = 15.f;

// The hostile entity closest to `lead`'s boresight: within maxRangeM, inside a halfAngleRad cone
// about `viewAxis`, ranked by smallest angular offset (ties by range). `viewAxis` is a normalized
// world-space direction; a degenerate (zero-length) axis falls back to the lead's body-forward axis.
//
// Returns an invalid EntityId when nothing qualifies. The caller MUST surface that as a refusal
// (WingmanResult::NoTarget) and must NOT silently substitute another target — ordering an attack on
// something the lead did not designate is worse than declining, exactly as the eval suite's
// out-of-grammar cases argue.
//
// `si` (the per-tick SpatialIndex) bounds the candidate scan when supplied; nullptr falls back to a
// full EntityManager::forEach, which is acceptable because designation happens on an operator action
// (a radio call), not every tick.
[[nodiscard]] fl::EntityId designateBoresightTarget(const fl::EntityManager& em, const fl::EntityState& lead,
                                                    const float viewAxis[3], float maxRangeM, float halfAngleRad,
                                                    const fl::SpatialIndex* si = nullptr);

// Nearest entity hostile to `selfFaction` within rangeM of `anchor`. Used by the engage/cover state
// factories to pick a threat AT STATE ENTRY (StateMachineController factories take no arguments, so
// the threat must be chosen inside the factory body — which is the right semantics: re-entering
// `engage` finds a fresh bandit rather than chasing a corpse).
[[nodiscard]] fl::EntityId nearestHostileWithin(const fl::EntityManager& em, const fl::EntityState& anchor,
                                                uint16_t selfFaction, float rangeM,
                                                const fl::SpatialIndex* si = nullptr);

} // namespace fl::ai
