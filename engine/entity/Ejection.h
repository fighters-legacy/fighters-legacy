// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Ejection, pilot survival, and outcome consequences (#672) — the pure model that separates the PILOT
// from the airframe. When an aircraft dies the entity dies; this decides what happens to the person in
// the seat, which is what lets the campaign (#584/#635) and the logbook (#674) express MIA/KIA/rescued
// rather than a binary alive/dead.
//
// Two pure decisions, both deterministic and unit-testable:
//   1. ejectionSurvivable(envelope) — did the seat get the pilot out alive? A zero-zero seat has an
//      envelope; punching out too low while pointing down (or far outside the seat limits) is fatal.
//   2. pilotOutcome(survived, landingZone) — where a surviving pilot comes down decides the outcome:
//      friendly territory rescues, hostile territory captures, no-man's-land is MIA. A dead seat is KIA.
//
// This header has no dependencies (plain scalars), so it lives in engine-entity and both the server
// (WorldBroadcaster ejection path) and tests use it without pulling engine-campaign.

#include <cstdint>

namespace fl {

// Whose ground the pilot's parachute came down over — resolved by the caller from theater/frontline
// control (engine-campaign's FrontlineControl maps onto this, but this header stays independent of it).
enum class TerritoryControl : uint8_t { Friendly, Neutral, Hostile };

// The consequence fed to the debrief (#634) and the campaign (#584). KIA and a survived ejection are
// deliberately distinct outcomes so career stakes and campaign branches can turn on them.
enum class EjectionOutcome : uint8_t {
    KIA,      // the pilot did not survive the seat (or never ejected before the aircraft was destroyed)
    Rescued,  // survived and came down over friendly territory
    MIA,      // survived but came down over no-man's-land / uncertain territory
    Captured, // survived but came down over hostile territory
};

// The seat's survivability envelope at the moment of ejection.
struct EjectionEnvelope {
    float altitudeAglM{0.f}; // height above ground level (m)
    float speedMs{0.f};      // airspeed (m/s)
    float sinkRateMs{0.f};   // downward velocity (m/s, positive = descending)
};

// Seat limits (a modern zero-zero seat has generous but finite limits). Defaults are a reasonable
// generic seat; content could tune them per airframe later.
struct EjectionSeatLimits {
    float maxSpeedMs{330.f};    // ~640 kt: beyond this the windblast is not survivable
    float minRecoveryAltM{0.f}; // a zero-zero seat works at zero altitude in level flight
    // Diving into the ground needs altitude for the chute to deploy: required AGL scales with sink rate.
    float chuteDeploySecondsAtSink{1.5f}; // seconds of descent the chute needs before ground contact
};

// True if a pilot ejecting in this envelope survives the seat. A zero-zero seat gets the pilot out on
// the runway in level flight, but not while diving into the ground below the chute-deploy altitude, and
// not above the windblast speed limit.
[[nodiscard]] inline bool ejectionSurvivable(const EjectionEnvelope& env,
                                             const EjectionSeatLimits& limits = {}) noexcept {
    if (env.speedMs > limits.maxSpeedMs)
        return false; // windblast
    // If descending, the chute needs enough altitude to deploy before ground contact.
    if (env.sinkRateMs > 0.f) {
        const float requiredAglM = env.sinkRateMs * limits.chuteDeploySecondsAtSink;
        if (env.altitudeAglM < requiredAglM)
            return false;
    }
    if (env.altitudeAglM < limits.minRecoveryAltM)
        return false;
    return true;
}

// Map (did the seat save the pilot?, where they landed) to the campaign/debrief outcome.
[[nodiscard]] inline EjectionOutcome pilotOutcome(bool survivedSeat, TerritoryControl landingZone) noexcept {
    if (!survivedSeat)
        return EjectionOutcome::KIA;
    switch (landingZone) {
    case TerritoryControl::Friendly:
        return EjectionOutcome::Rescued;
    case TerritoryControl::Hostile:
        return EjectionOutcome::Captured;
    case TerritoryControl::Neutral:
        break;
    }
    return EjectionOutcome::MIA;
}

// Did the pilot survive (in any form) for stats purposes — Rescued/MIA/Captured are all "pilot alive",
// only KIA is a career loss.
[[nodiscard]] inline bool pilotSurvived(EjectionOutcome o) noexcept {
    return o != EjectionOutcome::KIA;
}

[[nodiscard]] inline const char* ejectionOutcomeName(EjectionOutcome o) noexcept {
    switch (o) {
    case EjectionOutcome::KIA:
        return "KIA";
    case EjectionOutcome::Rescued:
        return "rescued";
    case EjectionOutcome::MIA:
        return "MIA";
    case EjectionOutcome::Captured:
        return "captured";
    }
    return "KIA";
}

} // namespace fl
