// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"

namespace fl {

class EntityManager;

// The gameplay gates every damage source funnels through (#626). A plain POD rather than the
// config-layer GameplayToggles so engine-entity does not grow a dependency on engine-config —
// fl-server maps its difficulty configuration into this at startup and on reload.
struct DamageRules {
    bool friendlyFire{false}; // false = same-faction damage from another entity is suppressed
    bool crashDamage{true};   // false = ground impacts report but do not damage
};

// THE single entry point for combat damage (#626). Every source — gunfire, warheads, collisions,
// crash impacts — applies through here rather than calling EntityManager::applyDamage directly,
// because this is where the gameplay gates live and a source that bypasses it silently ignores
// the server's difficulty settings.
//
// The friendly-fire gate: with rules.friendlyFire false, damage from an instigator that shares the
// target's non-zero faction is suppressed (returns false). Faction 0 is NEUTRAL, not a team — two
// neutral entities are not teammates, so neutral-on-neutral damage always applies. Self-damage
// (instigator == target) always applies: flying into your own blast radius is not friendly fire.
// Environmental damage (null instigator) always applies.
//
// Returns true when damage was applied, false when a gate suppressed it — callers use this to skip
// cosmetic-only side effects that would otherwise imply a hit that never happened.
//
// Sim-thread only (it can fire event handlers and kill, like everything on EntityManager).
bool applyPointDamage(EntityManager& em, EntityId target, float amount, EntityId instigator, const DamageRules& rules);

} // namespace fl
