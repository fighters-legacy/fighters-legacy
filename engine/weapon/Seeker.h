// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "sensor/Detection.h"
#include "weapon/WeaponDef.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>

namespace fl {

struct EntityState;

// The in-flight seeker of ONE missile against ONE designated target (#627). A missile is
// deliberately NOT a full SensorSystem observer — a 32-slot ContactTable per missile would be
// waste, and a seeker's whole job is a single track. It runs the SAME pure detection functions
// (`evaluateSensor` → `stepContact`) at the same 10 Hz reference cadence, staggered by projectile
// id, so all of honest sensing's rules bind seekers too: PoD gates acquisition, geometry maintains,
// a coast reports where the target WAS, and every die is the deterministic (observer, target, tick)
// hash — bit-identical across worker counts, platforms, and replays.
struct SeekerTrack {
    EntityId targetId;          // invalid = flying dumb (no designation, or the launch gate failed)
    sensor::ContactTrack track; // the shared Detection.h contact state machine
    glm::dvec3 lastKnownPos{};  // guidance steers at THIS — updated only when the target is actually
    glm::vec3 lastKnownVel{};   // SEEN; while Coasting the point is frozen (the target's last truth)
    uint64_t lastCheckTick{0};
    bool everChecked{false};
};

// Countermeasure seam (#529). Called once per seeker CHECK; returning true means an expendable
// seduced the seeker for this check — the target is treated as outside every lobe, so the track
// starts coasting (and reacquires by geometry when the seduction ends, per the central rule).
// Default null = no expendables in the world, which is exactly today's truth.
using SeekerCountermeasureCheck = std::function<bool(EntityId missile, EntityId target, uint64_t tickIndex)>;

// The sensor channel a seeker type observes — used by the legacy-lobe synthesizer and by callers
// mapping SeekerType onto the one sensor vocabulary.
[[nodiscard]] sensor::SensorType sensorTypeFor(SeekerType type) noexcept;

// Synthesize a SensorDef from a DEPRECATED legacy [seeker] lobe (fov_deg / acquisition_nm), so old
// packs keep flying for the one release of grace the #676 migration granted. Symmetric cone at the
// authored half-angle, track lobe slightly wider (a gimbal holds more than it acquires), moderate
// PoD, a short lock hold. Callers should prefer `sensor_id`.
[[nodiscard]] sensor::SensorDef synthesizeLegacySeekerDef(const SeekerDef& s);

// One seeker CHECK (the 10 Hz cadence): geometry + dice for the designated target, through the
// shared contact state machine. `target` may be null (dead / despawned) — the track then coasts
// out honestly instead of dropping instantly, exactly like any other sensor losing its geometry.
// `seduced` is the countermeasure seam's verdict for this check. `dtS` is the wall time since the
// previous check for THIS seeker. Updates `st.lastKnownPos/Vel` only when the target was actually
// inside a lobe this check.
void stepSeekerCheck(SeekerTrack& st, const sensor::SensorDef& def, bool emitting, const glm::dvec3& missilePos,
                     const float missileQuat[4], const EntityState* target, const SignatureDef& targetSig,
                     const sensor::SensingEnvironment& env, uint32_t missileIdx, uint64_t tickIndex, float dtS,
                     bool seduced);

} // namespace fl
