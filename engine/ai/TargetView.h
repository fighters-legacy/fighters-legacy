// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/AiTickContext.h"
#include "entity/EntityId.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "sensor/SensorSystem.h"

namespace fl::ai {

// What a controller is allowed to know about its target this tick.
//
// EVERY targeting controller goes through here, and that is the point: it is the single place where
// "where is my target" is answered, so a controller cannot accidentally reach past the sensors into
// the EntityManager. Pursuit, evade, break-turn, lead/lag and the yo-yos all call this instead of
// `em.get(targetId)->transform`.
struct TargetView {
    bool valid{false};   // false = do not steer at anything (no contact, or target dead)
    bool stale{false};   // true = this is a COASTING contact: a guess, not an observation
    double pos[3]{};     //
    float vel[3]{};      //
    uint16_t faction{0}; //
};

// Resolves a target through the honest path when sensing ran, and through ground truth when it did
// not.
//
// - `ctx.contacts == nullptr` — sensing was NOT evaluated (a unit test, a headless caller). Falls
//   back to the EntityManager, which is exactly the pre-sensing behavior. This is what keeps every
//   existing controller test valid and is the normative meaning of a null context field (#681).
// - `ctx.contacts != nullptr` — the honest path. The target must be a CONTACT. If the observer has
//   not detected it, `valid` is false and the controller flies as it does for a dead target: it does
//   not chase what it cannot see. A coasting contact returns its LAST-KNOWN state with `stale` set —
//   the controller may still steer at it (that is what a coast is *for*), but it is steering at a
//   memory, and it is told so.
[[nodiscard]] inline TargetView resolveTarget(const EntityManager& em, const AiTickContext& ctx, EntityId targetId) {
    TargetView v;

    if (!ctx.contacts) {
        const EntityState* st = em.get(targetId);
        if (!st || st->dead)
            return v;
        v.valid = true;
        v.pos[0] = st->transform.pos[0];
        v.pos[1] = st->transform.pos[1];
        v.pos[2] = st->transform.pos[2];
        v.vel[0] = st->transform.vel[0];
        v.vel[1] = st->transform.vel[1];
        v.vel[2] = st->transform.vel[2];
        v.faction = st->factionIndex;
        return v;
    }

    const sensor::Contact* c = ctx.contacts->find(targetId);
    if (!c || !c->held())
        return v; // not detected: there is nothing to steer at

    v.valid = true;
    v.stale = (c->state == sensor::ContactState::Coasting);
    v.pos[0] = c->lastKnownPos[0];
    v.pos[1] = c->lastKnownPos[1];
    v.pos[2] = c->lastKnownPos[2];
    v.vel[0] = c->lastKnownVel[0];
    v.vel[1] = c->lastKnownVel[1];
    v.vel[2] = c->lastKnownVel[2];
    v.faction = c->factionIndex;
    return v;
}

} // namespace fl::ai
