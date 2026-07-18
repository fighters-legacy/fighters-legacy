// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityManager.h"
#include "entity/IEntityController.h"

namespace fl::ai {

// Static air-defense controllers (#863): the SAM launcher and the AAA gun, the "shoots back" half of
// the builtin surface content. Both are EMPLACEMENTS — they do not move or steer (surface-vehicle
// movement is #585), so `sample` never touches throttle/aileron/elevator; it only decides when to
// pull the trigger. Both engage HONESTLY: the target comes from the emplacement's own contact table
// (designateFromContacts), so a SAM that a jammer has blinded engages nothing, exactly like a player.
// A null contact table (a caller that did not run sensing) means "no engagement" — an emplacement
// cannot ground-truth its way to a shot.
//
// LAUNCHER ELEVATION (#585 → #966/#970): these emplacements still fire along their FIXED nose. The
// primitive that closes the "a ground SAM needs launcher elevation" gap now exists — a turret mount
// with a slew servo (`engine/weapon/Turret.h`) and a directional launch vector (FireRequest::aimDir).
// An emplacement gets elevation by mounting its launcher as a turret and slewing it via that servo;
// wiring the SAM/AAA controllers onto a turret seat is the crewed-control follow-on (#969/#971).

// Acquire an aircraft on radar and launch a SARH at it. The launcher is fixed, so it engages targets
// within a forward cone of its facing (where the missile can turn to intercept); the emitting
// builtin:sam-radar sees the whole hemisphere, but firing is gated to the reachable arc. Launches on
// an interval (edge-triggered release, spaced past the launcher reload) while a target is held.
class SamEngagementController : public fl::IEntityController {
  public:
    SamEngagementController(const fl::EntityManager& entityManager, float engageRangeM = 30000.f,
                            float coneHalfAngleDeg = 90.f, float fireIntervalS = 4.f);

    fl::ControlInput sample(const fl::EntityState& state, uint64_t tick, double dt,
                            const fl::AiTickContext& ctx = {}) override;

  private:
    // Held for symmetry with AaaFireController (and a future lead-the-target enhancement); the SAM
    // itself engages purely off the contact table, so the reference is currently unused.
    [[maybe_unused]] const fl::EntityManager& m_entityManager;
    float m_engageRangeM;
    float m_coneHalfRad;
    uint64_t m_fireIntervalTicks;
    uint64_t m_lastFireTick{0};
    bool m_hasFired{false};
};

// Lead an aircraft with the ballistic gun solution and fire when the predicted miss at the target's
// range is inside the lethal radius AND the target is inside the gun's reach and engagement cone. A
// fixed emplacement, so it only hits what crosses its boresight — trigger discipline (no spraying at
// a target it cannot solve) is the same rule GunsEmploymentController holds, minus the steering.
class AaaFireController : public fl::IEntityController {
  public:
    AaaFireController(const fl::EntityManager& entityManager, float engageRangeM = 1200.f,
                      float coneHalfAngleDeg = 25.f, float muzzleVelMps = 1000.f, float lethalRadiusM = 15.f);

    fl::ControlInput sample(const fl::EntityState& state, uint64_t tick, double dt,
                            const fl::AiTickContext& ctx = {}) override;

  private:
    const fl::EntityManager& m_entityManager;
    float m_engageRangeM;
    float m_coneHalfRad;
    float m_muzzleVelMps;
    float m_lethalRadiusM;
};

} // namespace fl::ai
