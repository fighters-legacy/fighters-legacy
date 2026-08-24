// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/TurretGunnerController.h"

#include "ai/GunLead.h" // the shared gun lead preamble (#1265)
#include "ai/PerInstanceSkill.h"
#include "ai/TargetView.h"
#include "ai/Threat.h"
#include "config/DifficultySettings.h" // AiScaling — aimErrorDeg / reactionTimeS (this is its first consumer)
#include "flight/BallisticLead.h"
#include "flight/LocalFrame.h"
#include "weapon/Turret.h" // turretAimToAzEl (header-inline; no engine-weapon link)

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace fl::ai {

namespace {

// A deterministic uniform in [-1, 1] from (entity, seat, tick, salt) — the turbulence/dispersion idiom
// so a replayed burst jitters identically across worker counts.
[[nodiscard]] float signedNoise(uint32_t entityIdx, uint32_t seatIdx, uint64_t tick, uint32_t salt) noexcept {
    uint32_t h = entityIdx * 0x9E3779B1u + seatIdx * 0x85EBCA77u + static_cast<uint32_t>(tick) * 0xC2B2AE3Du +
                 static_cast<uint32_t>(tick >> 32) * 0x27D4EB2Fu + salt * 0x165667B1u;
    h = h * 1664525u + 1013904223u;
    h ^= h >> 15;
    return (static_cast<float>((h >> 8) & 0xFFFFu) / 65535.f - 0.5f) * 2.f;
}

} // namespace

TurretGunnerController::TurretGunnerController(const EntityManager& entityManager, float skillMin, float skillMax,
                                               float engageRangeM, float muzzleVelMps, float lethalRadiusM,
                                               uint64_t missionSeed)
    : m_entityManager(entityManager), m_skillMin(std::clamp(skillMin, 0.f, 1.f)),
      m_skillMax(std::clamp(std::max(skillMin, skillMax), 0.f, 1.f)), m_engageRangeM(engageRangeM),
      m_muzzleVelMps(muzzleVelMps), m_lethalRadiusM(lethalRadiusM), m_missionSeed(missionSeed) {}

SeatCommand TurretGunnerController::sample(const EntityState& airframe, const SeatView& seat, uint64_t tick, double dt,
                                           const AiTickContext& ctx) {
    SeatCommand cmd; // no aim, no fire until we designate a target

    // Roll this instance's skill once, from the deterministic per-instance seed.
    if (!m_skillRolled) {
        m_skill =
            rollPerInstanceSkill(skillSeed(m_missionSeed, airframe.id.index, seat.seatIndex), m_skillMin, m_skillMax);
        m_skillRolled = true;
    }

    // Honest sensing: engage only what the airframe has actually detected (a null table means sensing
    // was not evaluated — no engagement). A gunner without a turret has nothing to aim.
    if (!ctx.contacts || !seat.turret.present) {
        m_engaged = false;
        return cmd;
    }

    const glm::quat airQ{airframe.transform.quat[3], airframe.transform.quat[0], airframe.transform.quat[1],
                         airframe.transform.quat[2]};

    // Designate the nearest detected hostile in the turret's coverage arc: the look axis is the
    // turret's rest direction in world (aft for a tail gun), the cone is wide (the turret traverses
    // within it). designateFromContacts needs a real axis and does its own range/cone/hostility gate.
    const glm::vec3 restDir = glm::normalize(airQ * (seat.turret.mountRest * glm::vec3{1.f, 0.f, 0.f}));
    const float axis[3] = {restDir.x, restDir.y, restDir.z};
    const float coneHalfRad = 2.2f; // ~126 deg: a defensive gunner scans a wide arc
    const EntityId target = designateFromContacts(airframe, axis, ctx.contacts, m_engageRangeM, coneHalfRad, nullptr);
    if (!target.valid()) {
        m_engaged = false;
        return cmd;
    }

    // Lead the CONTACT's last-known state (never ground truth).
    const TargetView tv = resolveTarget(m_entityManager, ctx, target);
    if (!tv.valid) {
        m_engaged = false;
        return cmd;
    }

    const GunLeadSolution sol = leadSolution(airframe, tv, m_muzzleVelMps, m_planetRadiusM);
    const glm::dvec3 aimAt = sol.lead.valid ? sol.lead.aimPoint : sol.tgtPos;

    glm::vec3 want = glm::vec3(aimAt - sol.ownPos);
    const float wantLen = glm::length(want);
    if (wantLen < 1e-3f) {
        m_engaged = false;
        return cmd;
    }
    want /= wantLen;

    // Aim error from the rolled skill (the first consumer of AiScaling::aimErrorDeg). Perturb the aim
    // by a skill-scaled angle — deterministically hashed, and SLOWLY VARYING (held for a fraction of a
    // second) so the servo can settle on the biased aim and the gunner opens fire; a higher skill
    // shrinks the cone the rounds wander in. A rookie's rounds scatter wide; an ace's cluster tight.
    const float baseErrDeg = ctx.difficulty ? ctx.difficulty->aimErrorDeg : 3.f;
    const float errRad = baseErrDeg * (1.f - m_skill) * (3.14159265f / 180.f);
    if (errRad > 1e-5f) {
        constexpr uint64_t kJitterHoldTicks = 15; // ~0.25 s per aim step
        const uint64_t bucket = tick / kJitterHoldTicks;
        // A stable orthonormal basis around `want` for the two jitter axes.
        const glm::vec3 ref = std::abs(want.y) < 0.99f ? glm::vec3{0.f, 1.f, 0.f} : glm::vec3{1.f, 0.f, 0.f};
        const glm::vec3 right = glm::normalize(glm::cross(want, ref));
        const glm::vec3 up = glm::cross(right, want);
        const float e1 = signedNoise(airframe.id.index, seat.seatIndex, bucket, 0x11u) * errRad;
        const float e2 = signedNoise(airframe.id.index, seat.seatIndex, bucket, 0x22u) * errRad;
        want = glm::normalize(want + right * std::tan(e1) + up * std::tan(e2));
    }

    cmd.hasAim = true;
    cmd.aimDirWorld = want; // command the turret onto the (jittered) lead — the servo slews/clamps it

    // Reachability: if the true aim is outside the turret's arc, keep aiming (the servo clamps) but do
    // not fire — the rounds would leave along the clamped bore, not at the target.
    const glm::vec3 aimMount = glm::conjugate(seat.turret.mountRest) * (glm::conjugate(airQ) * want);
    float az = 0.f;
    float el = 0.f;
    turretAimToAzEl(aimMount, az, el);
    const bool reachable = az >= seat.turret.azMinRad && az <= seat.turret.azMaxRad && el >= seat.turret.elMinRad &&
                           el <= seat.turret.elMaxRad;

    // Reaction delay from the rolled skill: a rookie is slow off the mark. Times from the first tick
    // of a continuous engagement.
    if (!m_engaged) {
        m_engaged = true;
        m_engageStartTick = tick;
    }
    const float baseReactionS = ctx.difficulty ? ctx.difficulty->reactionTimeS : 1.0f;
    const float reactionS = baseReactionS * (1.f - m_skill);
    const bool reacted = (static_cast<double>(tick - m_engageStartTick) * dt) >= static_cast<double>(reactionS);

    // Trigger discipline: fire only when reachable, past the reaction delay, in range, and the turret's
    // CURRENT bore is within the lethal cone of the aim (i.e. the servo has actually pointed the gun on
    // target — hold fire while slewing).
    const float rangeM = sol.rangeM();
    if (reachable && reacted && rangeM > 1.f && rangeM <= m_engageRangeM) {
        cmd.trigger = fl::withinLethalMiss(glm::normalize(seat.turret.boreWorld), want, rangeM, m_lethalRadiusM);
    }
    return cmd;
}

} // namespace fl::ai
