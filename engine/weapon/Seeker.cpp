// SPDX-License-Identifier: GPL-3.0-or-later
#include "weapon/Seeker.h"

#include "entity/EntityState.h"

#include <algorithm>

namespace fl {

sensor::SensorType sensorTypeFor(SeekerType type) noexcept {
    switch (type) {
    case SeekerType::ActiveRadar:
    case SeekerType::SemiActiveRadar:
    case SeekerType::AntiRadiation:
        return sensor::SensorType::Radar;
    case SeekerType::Infrared:
        return sensor::SensorType::Ir;
    case SeekerType::Laser:
        return sensor::SensorType::Laser;
    case SeekerType::Gps:
    case SeekerType::Unguided:
        break;
    }
    return sensor::SensorType::Visual; // GPS/unguided never evaluate a lobe; value is inert
}

sensor::SensorDef synthesizeLegacySeekerDef(const SeekerDef& s) {
    sensor::SensorDef d;
    d.id = "legacy:seeker-lobe";
    d.name = "Legacy Seeker Lobe";
    d.type = sensorTypeFor(s.type);
    d.omnidirectional = false;
    d.emitter = (s.type == SeekerType::ActiveRadar);

    const float half = std::clamp(s.fovDeg > 0.f ? s.fovDeg : 30.f, 1.f, 180.f);
    d.search.azHalfAngleDeg = half;
    d.search.elHalfAngleDeg = half;
    d.search.minRangeM = 0.f;
    d.search.maxRangeM = s.acquisitionRangeM > 0.f ? s.acquisitionRangeM : 9260.f;
    d.search.pod = 0.5f;

    sensor::SensorLobe track = d.search;
    track.azHalfAngleDeg = std::min(180.f, half + 10.f); // the gimbal holds more than it acquires
    track.elHalfAngleDeg = track.azHalfAngleDeg;
    track.pod = 0.85f;
    d.track = track;
    d.lockHoldS = 1.0f;
    return d;
}

void stepSeekerCheck(SeekerTrack& st, const sensor::SensorDef& def, bool emitting, const glm::dvec3& missilePos,
                     const float missileQuat[4], const EntityState* target, const SignatureDef& targetSig,
                     const sensor::SensingEnvironment& env, uint32_t missileIdx, uint64_t tickIndex, float dtS,
                     bool seduced) {
    sensor::SensorEvaluation eval{}; // all-false: a dead target or a seduced seeker sees nothing
    if (target && !target->dead && !seduced) {
        const double mp[3] = {missilePos.x, missilePos.y, missilePos.z};
        // skill 0.5 = unity (the seeker head has no crew); radarRangeFraction 1 — difficulty
        // scaling (#682) tunes AI crews, not ordnance already in the air.
        eval = sensor::evaluateSensor(def, emitting, mp, missileQuat, target->transform.pos, targetSig,
                                      /*skill=*/0.5f, env, /*radarRangeFraction=*/1.f, missileIdx, st.targetId.index,
                                      tickIndex, /*sensorSlot=*/0);
    }

    st.track = sensor::stepContact(st.track, eval, def.lockHoldS, dtS, tickIndex);
    st.lastCheckTick = tickIndex;
    st.everChecked = true;

    // Last-known state advances only while the target is actually observed. A Coasting track keeps
    // the frozen point — it reports where the target WAS, never where it is.
    if (target && (eval.searchInLobe || eval.trackInLobe) && st.track.state != sensor::ContactState::Lost) {
        st.lastKnownPos = {target->transform.pos[0], target->transform.pos[1], target->transform.pos[2]};
        st.lastKnownVel = {target->transform.vel[0], target->transform.vel[1], target->transform.vel[2]};
    }
}

} // namespace fl
