// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"

#include <functional>

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

class SpatialIndex;

// What a detonation does, stripped of the weapon vocabulary. engine-entity deliberately does not
// know about WeaponDef (the dependency runs the other way — see engine/CMakeLists.txt), so callers
// map WeaponDef::warhead into this POD. The same reason SignatureDef is an entity-side POD.
struct BlastSpec {
    float radiusM{0.f};  // lethal radius; damage falls off linearly to zero here
    float damage{0.f};   // applied at the centre
    bool nuclear{false}; // adds the EMP ring at kEmpRadiusMultiple × radiusM
};

// Area-of-effect warhead detonation (#356) — the blast-radius damage query the per-entity damage
// model never had. Broadphase through the spatial index (cell-conservative), exact 3D range check,
// then LINEAR falloff: damage × (1 − d/R), full at the centre, zero at the edge. Every victim goes
// through applyPointDamage, so the friendly-fire gate holds inside a blast exactly as it does for
// a bullet — and self-damage applies, because your own blast radius does not care who armed it.
//
// Nuclear: entities within the EMP radius — kEmpRadiusMultiple × blast radius — take an avionics
// kill via `empEffect` (the caller wires it to SensorSystem::setAvionicsFailed; null = no EMP
// consumer in this context). The EMP applies to blast survivors and bystanders alike: electronics
// do not care about shrapnel range. Flash and cloud are client-side cosmetics and ride the effects
// channel, not this function.
//
// Deterministic: no dice anywhere — blast damage is geometry. Sim-thread only.
inline constexpr float kEmpRadiusMultiple = 4.f;

struct WarheadResult {
    int damaged{0}; // entities that took blast damage (post-gates)
    int emped{0};   // entities inside the EMP radius (nuclear only)
};

// `onVictim` (#675) fires for each entity that took blast damage, with the applied amount and the
// blast-to-victim direction — the caller routes it to per-subsystem damage. Null = no consumer.
using WarheadVictimHook = std::function<void(EntityId victim, float amount, const float hitDirWorld[3])>;

WarheadResult applyWarhead(EntityManager& em, const SpatialIndex& si, const double pos[3], const BlastSpec& blast,
                           EntityId instigator, const DamageRules& rules,
                           const std::function<void(EntityId)>& empEffect = {}, const WarheadVictimHook& onVictim = {});

} // namespace fl
