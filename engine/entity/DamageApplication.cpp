// SPDX-License-Identifier: GPL-3.0-or-later
#include "entity/DamageApplication.h"

#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "spatial/SpatialIndex.h"

#include <cmath>
#include <vector>

namespace fl {

bool applyPointDamage(EntityManager& em, EntityId target, float amount, EntityId instigator, const DamageRules& rules) {
    if (!target.valid() || amount <= 0.f)
        return false;

    const EntityState* victim = em.get(target);
    if (!victim || victim->dead)
        return false;

    if (!rules.friendlyFire && instigator.valid() && !(instigator == target)) {
        const EntityState* shooter = em.get(instigator);
        if (shooter && shooter->factionIndex != 0 && shooter->factionIndex == victim->factionIndex)
            return false; // teammates; the shot connected but the server does not honor it
    }

    em.applyDamage(target, amount, instigator);
    return true;
}

WarheadResult applyWarhead(EntityManager& em, const SpatialIndex& si, const double pos[3], const BlastSpec& blast,
                           EntityId instigator, const DamageRules& rules,
                           const std::function<void(EntityId)>& empEffect) {
    WarheadResult result;
    if (blast.radiusM <= 0.f || blast.damage <= 0.f)
        return result;

    const double blastR = static_cast<double>(blast.radiusM);
    const double outerR = blast.nuclear ? blastR * static_cast<double>(kEmpRadiusMultiple) : blastR;

    // Collect first, apply second: applyDamage can kill and fire handlers, and mutating the world
    // from inside the spatial-index visitation would be walking a structure while changing what it
    // describes. The candidate list is tiny (a blast radius, not a draw distance).
    struct Victim {
        EntityId id;
        double distM;
    };
    std::vector<Victim> victims;
    si.queryRadius(pos, outerR, [&](uint32_t idx, const double* p) {
        const double dx = p[0] - pos[0];
        const double dy = p[1] - pos[1];
        const double dz = p[2] - pos[2];
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > outerR * outerR)
            return; // the index is cell-conservative; the exact check is ours
        const EntityState* s = em.getByIndex(idx);
        if (!s || s->dead)
            return;
        victims.push_back({s->id, std::sqrt(d2)});
    });

    for (const Victim& v : victims) {
        if (v.distM <= blastR) {
            const float falloff = 1.f - static_cast<float>(v.distM / blastR);
            if (applyPointDamage(em, v.id, blast.damage * falloff, instigator, rules))
                ++result.damaged;
        }
        if (blast.nuclear && empEffect) {
            empEffect(v.id);
            ++result.emped;
        }
    }
    return result;
}

} // namespace fl
