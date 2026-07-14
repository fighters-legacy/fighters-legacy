// SPDX-License-Identifier: GPL-3.0-or-later
#include "weapon/ProjectileSystem.h"

#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "flight/IGravityField.h"
#include "spatial/SpatialIndex.h"
#include "weapon/ProjectileGuidance.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>

namespace fl {

namespace {

// Orient the projectile entity along its velocity: a quaternion rotating body +X onto v̂ with the
// smallest arc. Purely cosmetic (a 3-DOF point mass has no attitude of its own), but a missile
// rendered sideways reads as a bug.
void quatAlongVelocity(const glm::vec3& vel, float outQuat[4]) {
    const float speed = glm::length(vel);
    if (speed < 0.5f) {
        outQuat[0] = outQuat[1] = outQuat[2] = 0.f;
        outQuat[3] = 1.f;
        return;
    }
    const glm::vec3 fwd{1.f, 0.f, 0.f};
    const glm::vec3 dir = vel / speed;
    const float d = glm::dot(fwd, dir);
    glm::quat q{1.f, 0.f, 0.f, 0.f};
    if (d < -0.9999f) {
        q = glm::quat{0.f, 0.f, 1.f, 0.f}; // 180° about +Y: flying straight backwards
    } else if (d < 0.9999f) {
        const glm::vec3 axis = glm::cross(fwd, dir);
        q = glm::normalize(glm::quat{1.f + d, axis.x, axis.y, axis.z}); // smallest arc
    }
    outQuat[0] = q.x;
    outQuat[1] = q.y;
    outQuat[2] = q.z;
    outQuat[3] = q.w;
}

// The target's signature comes from its TYPE def — the same source SensorSystem reads, so a
// stealthy airframe is exactly as hard for a seeker as for the radar that found it. No registry
// (unit tests) = the baseline fighter (all 1.0).
SignatureDef signatureFor(const EntityTypeRegistry* registry, const EntityState& target) {
    if (registry) {
        if (const EntityDef* def = registry->byIndex(target.typeIndex))
            return def->signatures;
    }
    return SignatureDef{};
}

} // namespace

std::string projectileTypeId(const WeaponDef& def) {
    return "projectile:" + def.id;
}

EntityId ProjectileSystem::launch(EntityManager& em, uint32_t weaponIndex, const EntityState& shooterState,
                                  uint32_t shooterPeerOwnerId, EntityId designatedTarget) {
    if (!m_weapons)
        return EntityId::null();
    const WeaponDef* def = m_weapons->byIndex(weaponIndex);
    if (!def)
        return EntityId::null();

    // Launch state: the shooter's position nudged clear of its own contact-fuze bubble along the
    // nose, inheriting the shooter's velocity plus a separation push so the store never re-enters
    // the launch aircraft's fuze radius on release.
    const float* q = shooterState.transform.quat;
    const glm::quat rot{q[3], q[0], q[1], q[2]};
    const glm::vec3 nose = rot * glm::vec3{1.f, 0.f, 0.f};

    EntityTransform t{};
    for (int i = 0; i < 3; ++i)
        t.pos[i] = shooterState.transform.pos[i] + static_cast<double>(nose[i]) * 12.0;
    const glm::vec3 shooterVel{shooterState.transform.vel[0], shooterState.transform.vel[1],
                               shooterState.transform.vel[2]};
    const glm::vec3 v0 = shooterVel + nose * 15.f;
    for (int i = 0; i < 3; ++i)
        t.vel[i] = v0[i];
    quatAlongVelocity(v0, t.quat);

    const std::string typeId = projectileTypeId(*def);
    const EntityId id = em.spawn(typeId.c_str(), t, shooterPeerOwnerId);
    if (!id.valid())
        return id; // unknown projectile type (not registered) or pool refusal — already logged

    Projectile p;
    p.entityId = id;
    p.weaponIndex = weaponIndex;
    p.shooter = shooterState.id;
    p.pos = {t.pos[0], t.pos[1], t.pos[2]};
    p.vel = v0;
    p.launchPos = p.pos;
    p.motorRemainingS = def->performance.motorBurnTimeS;
    // Boost sized so an average burn reaches the weapon's stated max speed from a typical launch:
    // a = (v_max − |v0|) / t_burn. Def-derived and deterministic; refined per weapon class later
    // (#627 missiles, #629 bombs/rockets, #354 boosters).
    if (def->performance.motorBurnTimeS > 0.f && def->performance.maxSpeedMps > 0.f) {
        const float dv = std::max(0.f, def->performance.maxSpeedMps - glm::length(v0));
        p.thrustAccelMps2 = dv / def->performance.motorBurnTimeS;
    }
    p.armed = def->performance.minRangeM <= 0.f;

    // The launch acquisition gate (#627). A seeker weapon with a designated target resolves its
    // head (sensor_id through the shared resolver; the deprecated legacy lobe is synthesized) and
    // starts LOCKED only if the target is inside the acquisition lobe at release — the pre-launch
    // growl compressed to geometry; the PoD dice were paid during the shooter's own acquisition of
    // the target. A failed gate, a missing head, or no designation at all = the store flies dumb.
    if (def->seeker && def->seeker->type != SeekerType::Unguided && designatedTarget.valid()) {
        std::shared_ptr<const sensor::SensorDef> head;
        if (!def->seeker->sensorId.empty()) {
            if (m_sensorResolver)
                head = m_sensorResolver(def->seeker->sensorId);
        } else if (def->seeker->usesLegacyLobe()) {
            head = std::make_shared<const sensor::SensorDef>(synthesizeLegacySeekerDef(*def->seeker));
        }
        const EntityState* target = em.get(designatedTarget);
        if (head && target && !target->dead) {
            const SignatureDef sig = signatureFor(m_registry, *target);
            const float eff = sensor::effectiveMaxRangeM(head->search, head->type, sig);
            const double mp[3] = {p.pos.x, p.pos.y, p.pos.z};
            if (sensor::inLobe(mp, t.quat, target->transform.pos, head->search, head->omnidirectional, eff)) {
                p.seekerDef = std::move(head);
                p.seeker.targetId = designatedTarget;
                p.seeker.track.state = sensor::ContactState::Locked;
                p.seeker.track.lastSeenTick = 0;
                p.seeker.lastKnownPos = {target->transform.pos[0], target->transform.pos[1], target->transform.pos[2]};
                p.seeker.lastKnownVel = {target->transform.vel[0], target->transform.vel[1], target->transform.vel[2]};
                // A radiating head (ARH) stays quiet until pitbull (#628); everything else that
                // emits is active off the rail. IR is passive and never emits.
                p.emitting = p.seekerDef->emitter && def->seeker->pitbullRangeM <= 0.f;
            }
        }
    }

    m_projectiles.push_back(p);
    return id;
}

void ProjectileSystem::step(EntityManager& em, const SpatialIndex& si, float dt, const GroundQuery& ground,
                            uint64_t tickIndex, const sensor::SensingEnvironment& env,
                            std::vector<ProjectileImpact>& outImpacts) {
    if (!m_weapons || !m_gravity)
        return;

    for (auto it = m_projectiles.begin(); it != m_projectiles.end();) {
        Projectile& p = *it;
        const WeaponDef* def = m_weapons->byIndex(p.weaponIndex);
        EntityState* es = em.get(p.entityId);
        if (!def || !es || es->dead) {
            // The mirrored entity died out from under us (admin kill, soft-cap eviction): the
            // projectile just stops existing — no detonation for a shot the world deleted.
            it = m_projectiles.erase(it);
            continue;
        }

        // ── seeker check (#627): the 10 Hz reference cadence, staggered by projectile id ─────
        if (p.seekerDef && p.seeker.targetId.valid() && (tickIndex + p.entityId.index) % kSeekerCheckTicks == 0) {
            const EntityState* target = em.get(p.seeker.targetId); // gen-checked: a reused slot is null
            const bool seduced = m_cmCheck && target && m_cmCheck(p.entityId, p.seeker.targetId, tickIndex);
            const float checkDtS = p.seeker.everChecked ? static_cast<float>(tickIndex - p.seeker.lastCheckTick) * dt
                                                        : static_cast<float>(kSeekerCheckTicks) * dt;
            const SignatureDef sig = target ? signatureFor(m_registry, *target) : SignatureDef{};
            stepSeekerCheck(p.seeker, *p.seekerDef, p.emitting, p.pos, es->transform.quat, target, sig, env,
                            p.entityId.index, tickIndex, checkDtS, seduced);
        }

        // ── 3-DOF point-mass step ────────────────────────────────────────────
        const double posArr[3] = {p.pos.x, p.pos.y, p.pos.z};
        const std::array<float, 3> g = m_gravity->accelWorld(posArr);
        glm::vec3 accel{g[0], g[1], g[2]};

        const float speed = glm::length(p.vel);
        if (p.motorRemainingS > 0.f && speed > 0.01f) {
            accel += (p.vel / speed) * p.thrustAccelMps2; // boost along the flight path
            p.motorRemainingS -= dt;
        } else if (speed > 0.01f) {
            accel -= p.vel * kCoastDecayPerS; // simple coast decay; per-class drag arrives later
        }

        // Proportional navigation (#627) at the seeker's LAST-KNOWN state — never ground truth. A
        // Coasting track steers at the frozen point (where the target WAS); a Lost track flies
        // ballistically on whatever course guidance last set.
        if (p.seekerDef && p.seeker.targetId.valid() && p.seeker.track.state != sensor::ContactState::Lost) {
            const float maxAccel = (def->performance.maxG > 0.f ? def->performance.maxG : 30.f) * kGravityAccelMps2;
            accel += proportionalNavAccel(p.pos, p.vel, p.seeker.lastKnownPos, p.seeker.lastKnownVel,
                                          kProportionalNavGain, maxAccel);
        }

        const glm::dvec3 prevPos = p.pos;
        p.vel += accel * dt;
        p.pos += glm::dvec3(p.vel) * static_cast<double>(dt);
        p.ageS += dt;

        // ── arming ───────────────────────────────────────────────────────────
        if (!p.armed) {
            const glm::dvec3 fromLaunch = p.pos - p.launchPos;
            const double armDist = static_cast<double>(def->performance.minRangeM);
            p.armed = glm::dot(fromLaunch, fromLaunch) >= armDist * armDist;
        }

        // ── endings ──────────────────────────────────────────────────────────
        bool ended = false;
        ProjectileImpact impact{};
        impact.weaponIndex = p.weaponIndex;
        impact.shooter = p.shooter;

        // Proximity fuze: closest approach of this tick's flight SEGMENT to any non-shooter
        // entity, once armed. The segment test is what makes a Mach-4 closure honest — at
        // 25 m/tick a point-in-bubble check would tunnel straight through an 8 m fuze radius.
        if (p.armed) {
            const glm::dvec3 seg = p.pos - prevPos;
            const double segLen2 = glm::dot(seg, seg);
            const glm::dvec3 mid = (prevPos + p.pos) * 0.5;
            const double c[3] = {mid.x, mid.y, mid.z};
            const double queryR = 0.5 * std::sqrt(segLen2) + static_cast<double>(kContactFuzeM);
            double bestD2 = static_cast<double>(kContactFuzeM) * static_cast<double>(kContactFuzeM);
            uint32_t bestIdx = UINT32_MAX;
            glm::dvec3 bestAt = p.pos;
            si.queryRadius(c, queryR, [&](uint32_t idx, const double* ep) {
                if (idx == p.entityId.index || idx == p.shooter.index)
                    return;
                const glm::dvec3 e{ep[0], ep[1], ep[2]};
                // Closest point on [prevPos, pos] to the entity centre.
                double tSeg = 0.0;
                if (segLen2 > 1e-9)
                    tSeg = std::clamp(glm::dot(e - prevPos, seg) / segLen2, 0.0, 1.0);
                const glm::dvec3 at = prevPos + seg * tSeg;
                const glm::dvec3 d = e - at;
                const double d2 = glm::dot(d, d);
                if (d2 <= bestD2) {
                    bestD2 = d2;
                    bestIdx = idx;
                    bestAt = at;
                }
            });
            if (bestIdx != UINT32_MAX) {
                if (const EntityState* hit = em.getByIndex(bestIdx)) {
                    impact.directHit = hit->id;
                    p.pos = bestAt; // detonate at the fuze point, not past it
                    ended = true;
                }
            }
        }

        // Ground impact: radial altitude at or below the terrain under the projectile.
        if (!ended) {
            const double posNow[3] = {p.pos.x, p.pos.y, p.pos.z};
            const double alt = m_gravity->geodeticAltitude(posNow);
            const float terrain = ground ? ground(p.pos) : 0.f;
            if (alt <= static_cast<double>(terrain))
                ended = true;
        }

        // Overrange / TTL self-destruct: still a detonation (visibly, harmlessly high up usually),
        // never a silent vanish — players watch missiles, and missiles that blink out read as bugs.
        if (!ended) {
            const glm::dvec3 fromLaunch = p.pos - p.launchPos;
            const double maxR = static_cast<double>(def->performance.maxRangeM) * 1.5;
            if ((maxR > 0.0 && glm::dot(fromLaunch, fromLaunch) > maxR * maxR) || p.ageS > kMaxFlightTimeS)
                ended = true;
        }

        if (ended) {
            impact.pos[0] = p.pos.x;
            impact.pos[1] = p.pos.y;
            impact.pos[2] = p.pos.z;
            outImpacts.push_back(impact);
            em.kill(p.entityId); // despawn TLV + reap handle the rest; instigator-less: the
                                 // projectile is spent ordnance, not a scoring kill
            it = m_projectiles.erase(it);
            continue;
        }

        // ── mirror into the pooled entity ────────────────────────────────────
        es->transform.pos[0] = p.pos.x;
        es->transform.pos[1] = p.pos.y;
        es->transform.pos[2] = p.pos.z;
        es->transform.vel[0] = p.vel.x;
        es->transform.vel[1] = p.vel.y;
        es->transform.vel[2] = p.vel.z;
        quatAlongVelocity(p.vel, es->transform.quat);

        ++it;
    }
}

} // namespace fl
