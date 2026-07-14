// SPDX-License-Identifier: GPL-3.0-or-later
#include "entity/DamageApplication.h"

#include "entity/EntityManager.h"
#include "entity/EntityState.h"

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

} // namespace fl
