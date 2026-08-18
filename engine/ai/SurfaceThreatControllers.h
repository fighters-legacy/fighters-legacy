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
// LAUNCHER ELEVATION (#585 → #966/#970/#1204): the SAM no longer fires along its fixed nose. It
// hands the fire path a world-space launch vector (ControlInput::aimDir → FireRequest::aimDir)
// pointing at its designated contact, elevated to at least `launchElevationMinDeg` above the local
// horizon so the store clears the deck. Before #1204 it fired along the airframe nose, which for an
// emplacement sitting on flat ground is HORIZONTAL: the missile began its life at ground level and
// the projectile ground check reaped it within a few steps at every engagement range where the line
// of sight was shallow. Measured on `builtin:sam-site` at 3 km target altitude, before the fix:
// 4 km range → kill; 8 km → the store lived 39 ticks; 12 km → 28; 20 km and beyond → 24 ticks and
// no missile ever arrived. Every observable short of the missile itself looked correct.
//
// The AAA still fires along its fixed nose — it is a gun with no elevation, and giving it one is a
// traverse/elevation envelope question rather than a launch-geometry bug (its hitscan rounds are
// not reaped by the ground). Mounting either weapon on a turret seat with a slew servo
// (`engine/weapon/Turret.h`) remains the fuller fix and the crewed-control follow-on (#969/#971);
// a turret seat's bore overrides the vector set here.

// Acquire an aircraft on radar and launch a SARH at it. The launcher is fixed, so it engages targets
// within a forward cone of its facing (where the missile can turn to intercept); the emitting
// builtin:sam-radar sees the whole hemisphere, but firing is gated to the reachable arc. Launches on
// an interval (edge-triggered release, spaced past the launcher reload) while a target is held.
class SamEngagementController : public fl::IEntityController {
  public:
    // launchElevationMinDeg: the floor on how far above the local horizon the store leaves, applied
    // when the contact sits at or below that angle. A rail launcher elevates before it fires; 35
    // degrees is enough loft that the missile is clear of the deck well before proportional
    // navigation pulls it down onto a shallow line of sight, and it is inside the elevation arc a
    // real emplacement's launcher travels. Set it to 0 for a launcher that genuinely fires flat.
    SamEngagementController(const fl::EntityManager& entityManager, float engageRangeM = 30000.f,
                            float coneHalfAngleDeg = 90.f, float fireIntervalS = 4.f,
                            float launchElevationMinDeg = 35.f);

    fl::ControlInput sample(const fl::EntityState& state, uint64_t tick, double dt,
                            const fl::AiTickContext& ctx = {}) override;

  private:
    void setLaunchVector(fl::ControlInput& ctrl, const fl::EntityState& state, const fl::AiTickContext& ctx,
                         fl::EntityId target) const;

    // Resolves the designated contact to its LAST-KNOWN state (never ground truth) so the launch
    // vector points where this battery believes the target is (#1204).
    const fl::EntityManager& m_entityManager;
    float m_engageRangeM;
    float m_coneHalfRad;
    uint64_t m_fireIntervalTicks;
    float m_launchElevationMinRad;
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
