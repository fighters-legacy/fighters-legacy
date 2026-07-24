// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/CrewDef.h"
#include "entity/DamageDef.h"
#include "entity/DeckDef.h"
#include "entity/ObjectCategory.h"
#include "entity/SignatureDef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fl {

// One weapon station on an airframe. Hardpoints are a property of the ENTITY, not of its flight
// model: the flight model is aerodynamics, and what the aircraft is allowed to carry is not. The
// physics consequence of a loadout reaches the flight model through PayloadEffect
// (engine/flight/AeroForces.h), which is the only coupling that should exist.
//
// A station has NO kind of its own. `allowed` IS the compatibility contract: real multi-role
// stations carry bombs OR rocket pods OR drop tanks (an F-16's wet wing stations do exactly
// that), and the old single `type` enum could not say so -- it was redundant with `allowed`
// where it agreed and a lie where it did not. Whether a mounted store fires or is inert
// (Fuel/Pod) is a property of the WEAPON (WeaponDef::type), resolved per mount.
struct Hardpoint {
    int slot{0};                      // station number; unique within an entity
    std::vector<std::string> allowed; // weapon ids this station accepts; never empty
    std::string defaultWeapon;        // pre-loaded weapon id; must be a member of `allowed`
};

// Immutable definition for one entity type, loaded from a content pack TOML file and
// registered with EntityTypeRegistry. Shared by all live instances of the same type.
//
// TWO VOCABULARIES, AND THEY ARE NOT INTERCHANGEABLE (#810). A field naming a FILE is an ASSET NAME
// — a bare stem that FolderContentPack pastes into a path (`mesh`, `classicDamageMesh`,
// `flightModelAsset`, `aiScriptAsset`, `cockpitMesh`). A field naming a DEF is a NAMESPACED ID —
// resolved through ContentIndex, never through the filesystem (`id`, `sensorIds`,
// `Hardpoint::allowed`, `Hardpoint::defaultWeapon`). The `*Asset` / `*Id` suffixes say which is
// which, and they are load-bearing: passing an id to AssetManager builds
// "sensors/fl-base:apq159.toml", which cannot exist and is not a legal Windows filename.
struct EntityDef {
    std::string id; // content-pack-scoped def ID, e.g. "fl-base:f15c"
    std::string name;
    ObjectCategory category{ObjectCategory::AirVehicle};
    // Which weapon class a Projectile-category type flew off as (#886) — drives the per-class
    // builtin placeholder silhouette. None for every other category. Set from WeaponDef::type by
    // registerProjectileEntityDefs, or from the optional `projectile_kind` TOML key (default:
    // missile) for hand-authored projectile defs. Sent to the client on MsgEntityTypeDef.
    ProjectileKind projectileKind{ProjectileKind::None};
    float maxHp{100.f};
    std::optional<DamageDef> damage; // absent = binary death (no progressive damage)
    std::string mesh;                // ASSET NAME for primary geometry
    std::string cockpitMesh;         // ASSET NAME: cockpit interior geometry; empty if none (#813)
    std::string manualAsset;         // ASSET NAME: hand-written manual prose (#821); the numbers are generated
    std::string classicDamageMesh;   // ASSET NAME: JumpToDamage geometry variant; empty if none
    // Variant node-set selector (#882): which tagged node-set of a shared family mesh this type
    // draws. NOT an asset name and NOT a def id — it is a tag matched against the glTF nodes'
    // `extras.fl_variant`, so one .glb can carry a family's union of geometry (a MiG-21bis nose and a
    // MiG-21U two-seat canopy) and each entity def picks its set. Untagged nodes are always drawn, so
    // empty (the default) means "the shared airframe only" — every mesh authored before this existed.
    // Node PRESENCE, chosen statically at load; node POSE is articulation (#837), a different axis.
    std::string meshVariant;
    std::string flightModelAsset;      // ASSET NAME: flight-model TOML; empty = builtin UFO model
    std::string aiScriptAsset;         // ASSET NAME: Lua AI script; empty = no scripted AI (server-side)
    std::vector<Hardpoint> hardpoints; // weapon stations; empty = carries nothing

    // ── crew seats & turret mounts (#966) ────────────────────────────────────
    // Authored crew positions and turret mounts. EMPTY IS MEANINGFUL: an entity with no `crew` is
    // the implicit 1-seat case — a plain fighter, seats[0] being the pilot / today's single
    // controller path — so every existing def is a valid crewed aircraft with zero churn. When
    // non-empty the partition obeys the one-owner-per-channel invariant (validateCrewPartition,
    // enforced by parseEntityDef). See entity/CrewDef.h.
    std::vector<SeatDef> crew;
    std::vector<TurretDef> turrets;

    // ── resolved default loadout (#812) ──────────────────────────────────────
    // What the DEFAULT loadout costs the airframe, summed over `hardpoints` once at load time and
    // cached here. Plain floats rather than a PayloadEffect so engine-entity does not have to link
    // engine-flight -- the same reason sensorIds are plain strings. The server computes them from
    // the WeaponRegistry (fl::defaultPayload); the client receives them on MsgEntityTypeDef, because
    // it has no hardpoints and no weapon registry and must not need one to predict its own aircraft.
    float payloadMassKg{0.f};
    float payloadCd0{0.f};

    // ── sensing (#680) ───────────────────────────────────────────────────────
    // What the entity looks like to an observer. Defaults are the baseline fighter (all 1.0), so an
    // entity that says nothing is exactly as detectable as the numbers in a sensor def assume.
    SignatureDef signatures{};

    // Which sensors the entity carries, as sensor-def IDS (e.g. "fl-base:apg63"). Plain strings, so
    // engine-entity does not depend on engine-sensor; ids are resolved through ContentIndex to an
    // asset name at load time (#810 — they are NOT filenames), where an unknown one is an ERROR that
    // does not stop the spawn: an aircraft still loads with the rest of its suite, but a typo that
    // silently leaves it flying blind is not a warning-grade event.
    //
    // EMPTY IS MEANINGFUL: an AI-controlled entity with no declared sensors gets the builtin
    // eyeball, not omniscience and not blindness (2026-07-12 decision record). Honest sensing is the
    // default; a pack cannot opt out of it by leaving this list off.
    std::vector<std::string> sensorIds;

    // Per-unit acquisition tuning. Absent = the engine default (AiTuning{}), so authors tune only
    // the units they care about; an elite interceptor and a conscript SAM crew can fly identical
    // hardware and still behave differently.
    std::optional<AiTuning> aiTuning;

    // ── collision (#630) ─────────────────────────────────────────────────────
    // The entity's collision sphere radius (metres) for entity-entity collision detection. 0 = use
    // the category default (Air 8 m, Ground/Naval/Structure 15 m; Projectiles never collide here — they have
    // their own fuze path). A blimp or a carrier wants an explicit value; a fighter does not.
    float collisionRadiusM{0.f};

    // ── electronic warfare (#529) ────────────────────────────────────────────
    // Expendable countermeasure magazines: chaff (defeats radar seekers) and flare (defeats IR). 0 =
    // no dispenser of that kind. A fighter carries dozens; a transport or a ground unit none. ECM
    // (noise jamming) is a runtime toggle (EntityState::ecmActive), not a magazine — an aircraft with
    // a jammer switches it on and off; there is nothing to run out of.
    uint16_t chaffCount{0};
    uint16_t flareCount{0};

    // ── carrier / flight-deck seam (#699, consumed by #38) ───────────────────
    // Whether an aircraft may recover onto this entity's surface — the FA design lesson that made
    // "this surface accepts landings" a data property so a carrier reuses the airfield landing path
    // (AirportDef carries the same flag for land airfields). Default false: an ordinary vehicle is
    // not a runway.
    bool acceptsLandings{false};

    // The flight deck itself (#38): footprint, catapult and arrest-wire geometry, authored as
    // `[deck]` in the entity TOML. Only read when acceptsLandings is true. The footprint travels
    // to clients on MsgEntityTypeDef so prediction composes the same moving floor the server does.
    std::optional<DeckDef> deck;
};

// Category default collision radius (#630) — used when EntityDef::collisionRadiusM is 0.
// Projectiles return 0: they are excluded from the entity-entity phase entirely (the ProjectileSystem
// fuze is their collision model), and an Effect has no physical presence.
[[nodiscard]] inline float defaultCollisionRadiusM(ObjectCategory category) noexcept {
    switch (category) {
    case ObjectCategory::AirVehicle:
    case ObjectCategory::Player:
        return 8.f;
    case ObjectCategory::GroundVehicle:
    case ObjectCategory::NavalVehicle:
    case ObjectCategory::Structure:
        return 15.f;
    case ObjectCategory::Projectile:
    case ObjectCategory::Effect:
        return 0.f;
    }
    return 0.f;
}

} // namespace fl
