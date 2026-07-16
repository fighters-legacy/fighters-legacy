// SPDX-License-Identifier: GPL-3.0-or-later
#include "mission/MissionSetup.h"

#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "flight/LocalFrame.h" // enuBasis (local tangent frame)
#include "mission/Mission.h"
#include "weather/WeatherController.h"
#include "world/FactionRegistry.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <cmath>

namespace fl {

namespace {

// Build an orientation whose +X body-forward axis points at compass heading `headingDeg` (0 = north,
// clockwise, matching LocalFrame::headingOf) in the local tangent plane at `pos`, wings level (body up
// = radial up). Correct anywhere on the sphere; near the world origin it reduces to a plain world-Y
// yaw. Returned in EntityTransform quaternion order [x, y, z, w].
void yawHeadingToQuat(float headingDeg, const double pos[3], double R, float outQuat[4]) {
    const glm::dvec3 p(pos[0], pos[1], pos[2]);
    const glm::mat3 enu = enuBasis(p, R); // columns (East, North, Up)
    const float h = glm::radians(headingDeg);
    const glm::vec3 up = enu[2];
    // heading 0 = North, +90 = East (clockwise): forward = sin(h)*East + cos(h)*North.
    const glm::vec3 forward = glm::normalize(std::sin(h) * enu[0] + std::cos(h) * enu[1]);
    const glm::vec3 right = glm::normalize(glm::cross(forward, up)); // starboard (forward x up)
    // Body basis maps X->forward, Y->up, Z->right; columns of the rotation matrix are those images.
    const glm::mat3 basis(forward, up, right);
    const glm::quat q = glm::quat_cast(basis);
    outQuat[0] = q.x;
    outQuat[1] = q.y;
    outQuat[2] = q.z;
    outQuat[3] = q.w;
}

} // namespace

MissionSetupResult applyMission(const Mission& mission, EntityManager& em, FactionRegistry& factions,
                                WeatherController* weather, double planetRadiusM, const MissionSpawnHook& onSpawned) {
    MissionSetupResult result;

    // ── coalition registry ──────────────────────────────────────────────────────
    // Index 0 is reserved neutral (EntityState::factionIndex==0 means neutral, and areFactionsHostile
    // gives faction 0 no enemies), so real sides occupy 1..N and never land on the neutral index.
    std::vector<FactionDef> defs;
    defs.reserve(mission.sides.size() + 1);
    defs.push_back(FactionDef{/*id=*/"", /*name=*/"(neutral)"});
    for (const auto& side : mission.sides)
        defs.push_back(FactionDef{side.id, side.id});
    factions.load(std::move(defs));

    // Default every distinct pair of real sides to Hostile (the wargame default: you fight whoever you
    // are not allied with), then override declared allies to Friendly.
    const uint16_t n = factions.count();
    for (uint16_t a = 1; a < n; ++a)
        for (uint16_t b = static_cast<uint16_t>(a + 1); b < n; ++b)
            factions.setRelationship(a, b, FactionRelation::Hostile);
    for (const auto& side : mission.sides) {
        const uint16_t si = factions.indexOf(side.id);
        if (si == UINT16_MAX)
            continue;
        for (const auto& ally : side.allies) {
            const uint16_t ai = factions.indexOf(ally);
            if (ai != UINT16_MAX && ai != 0 && ai != si)
                factions.setRelationship(si, ai, FactionRelation::Friendly);
        }
    }

    // ── objects ─────────────────────────────────────────────────────────────────
    for (const auto& obj : mission.objects) {
        uint16_t fi = factions.indexOf(obj.side);
        if (fi == UINT16_MAX)
            fi = 0; // side not resolvable (defensive; the parser cross-checks) → neutral

        if (obj.playerSlot) {
            PlayerSlot slot;
            slot.id = obj.id; // for the destroy(<id>) binding once a pilot claims the slot (#884)
            slot.type = obj.type;
            slot.factionIndex = fi;
            slot.pos[0] = obj.pos[0];
            slot.pos[1] = obj.alt ? static_cast<double>(*obj.alt) : obj.pos[1];
            slot.pos[2] = obj.pos[2];
            slot.headingDeg = obj.headingDeg;
            slot.speed = obj.speed; // initial airspeed for the joining pilot (#883); absent = cruise default
            yawHeadingToQuat(obj.headingDeg, slot.pos, planetRadiusM, slot.quat);
            result.playerSlots.push_back(std::move(slot));
            continue;
        }

        EntityTransform t{};
        t.pos[0] = obj.pos[0];
        t.pos[1] = obj.alt ? static_cast<double>(*obj.alt) : obj.pos[1];
        t.pos[2] = obj.pos[2];
        yawHeadingToQuat(obj.headingDeg, t.pos, planetRadiusM, t.quat);

        const EntityId id = em.spawn(obj.type.c_str(), t);
        if (!id.valid()) {
            result.warnings.push_back("mission object '" + obj.id + "' (type '" + obj.type +
                                      "') failed to spawn — unregistered type or entity cap reached");
            continue;
        }
        if (EntityState* st = em.get(id))
            st->factionIndex = fi;
        result.spawned.push_back(id);
        result.objectEntities.emplace_back(obj.id, id); // for the #633 destroy(<id>) predicate

        // Hand the freshly spawned object to the caller so it can attach a controller (ai/route) and a
        // loadout override (#855) using seams engine-mission does not link.
        if (onSpawned)
            onSpawned(id, obj);
    }

    // ── weather / time / wind ─────────────────────────────────────────────────────
    if (weather) {
        if (mission.weatherPreset)
            weather->setPreset(*mission.weatherPreset);
        weather->setTimeOfDay(static_cast<float>(mission.time.hour) + static_cast<float>(mission.time.minute) / 60.f);
        weather->setWind(mission.wind.headingDeg, mission.wind.speedMs);
    }

    return result;
}

} // namespace fl
