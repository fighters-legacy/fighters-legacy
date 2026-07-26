// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fl {

class AssetManager;
class ContentIndex;
class EntityTypeRegistry;
class ILogger;
class WeaponRegistry;
struct AirportDef;
struct EntityDef;
struct EscalationPolicy;

namespace sensor {
struct SensorDef;
}

// Loads every content-pack entity definition into `registry` (#683).
//
// Enumerates AssetManager::listAssets(AssetType::EntityDef) (first-pack-wins dedup, matching asset
// priority), parses each via parseEntityDef, and registers it. A def that fails to load, fails to
// parse, or duplicates an already-registered id logs a Warn and is skipped -- it never aborts the
// caller. Must run on the main thread before GameLoop::start() (the registry is read-only during
// simulation). Returns the number of pack types successfully registered.
uint32_t registerPackEntityDefs(AssetManager& assets, EntityTypeRegistry& registry, ILogger& log);

// Loads every content-pack airport definition (#699) and APPENDS the parsed AirportDefs to `out`
// (it collects rather than registers, because AirportRegistry::load is load-once and the caller
// merges the builtin airfield + pack airports + the OurAirports CSV [#486] into one load()). Same
// warn-and-skip-on-failure shape as registerPackEntityDefs. First-id-wins is enforced by the
// registry; a pack that ships airports/*.toml is enumerated via listAssets(AssetType::Airport).
// Main thread, before GameLoop::start(). Returns the number of airports appended.
uint32_t registerPackAirportDefs(AssetManager& assets, std::vector<AirportDef>& out, ILogger& log);

// Loads every content-pack weapon definition into `registry` (#812), the same shape as
// registerPackEntityDefs above. Enumerates AssetManager::listAssets(AssetType::Weapon), parses each
// via parseWeaponDef, and registers it by its ID -- so a hardpoint resolves its stores without ever
// touching the filesystem. A def that fails to load, fails to parse, or duplicates an already
// registered id logs a Warn and is skipped; it never aborts the caller.
//
// Call BEFORE registerPackEntityDefs, so an entity's hardpoints have weapons to resolve against.
// Main thread, before GameLoop::start(). Returns the number of weapons registered.
// Loads every content-pack airspace escalation policy (#162) and APPENDS the parsed policies to
// `out`, the same collect-don't-register shape as registerPackAirportDefs (AlertSystem::addPolicy is
// the registration step, and the caller merges pack policies with any it synthesizes). A policy that
// fails to load or parse logs a Warn and is skipped; a zone naming a policy that never arrived falls
// back to the builtin default rather than going inert. Enumerated via listAssets(AssetType::ZonePolicy)
// -- a pack ships them as zones/<id>.toml. Main thread, before GameLoop::start(). Returns the count.
uint32_t registerPackZonePolicies(AssetManager& assets, std::vector<EscalationPolicy>& out, ILogger& log);

uint32_t registerPackWeaponDefs(AssetManager& assets, WeaponRegistry& registry, ILogger& log);

// Registers a projectile ENTITY type ("projectile:<weapon id>", ObjectCategory::Projectile) for
// every flyable weapon in the registry — missiles, rockets, bombs (#625). Guns are hitscan and get
// none. This must happen at startup because MsgEntityTypeDef travels ONLY in ConnectAck: a type
// registered after a client connects would reach it as an unresolvable typeIndex, and the client
// would render nothing where a missile is. The projectile's mesh is the weapon's `mesh` asset name
// (empty = builtin placeholder); its radar signature is small (a missile is a hard radar target to
// SEE, not to hit). Main thread, before GameLoop::start(). Returns the number registered.
uint32_t registerProjectileEntityDefs(const WeaponRegistry& weapons, EntityTypeRegistry& registry, ILogger& log);

// Registers the compiled-in sandbox weapons (#440) — BuiltinWeapon::cannon()/irMissile()/
// radarMissile() — into `registry`. Always safe to call alongside pack weapons: the "builtin:"
// namespace cannot collide with a pack id. Call BEFORE registerProjectileEntityDefs so the builtin
// missiles get projectile entity types. Returns the number registered.
uint32_t registerBuiltinWeapons(WeaponRegistry& registry);

// The builtin debug entity, ARMED (#440): one cannon, two IR rails, two radar rails, all builtin
// defaults. Shared by fl-server and the game client so the two can never drift — this is the type
// WorldBroadcaster::onConnect spawns per peer, and the def whose hardpoints size the client's
// weapon-station selector in the zero-pack sandbox.
EntityDef builtinDebugEntityDef();

// The builtin MULTI-CREW aircraft (#966/#977): a Fly+Fire+Radar+Countermeasures pilot dropping bombs
// off station 0, plus a defensive tail-gunner seat aiming a rear-facing cannon turret (station 1),
// filled by the builtin turret gunner (#971). The crewed counterpart to builtin:debug-entity — it
// makes the whole crew → seat-sampling → turret-slew → directional-fire path provable zero-pack.
EntityDef builtinBomberDef();

// Builtin surface targets and threats (#863) — so the ground/naval/static categories and a
// shoots-back air-defense threat exist zero-pack. Each carries a SignatureDef, a 3-level DamageDef
// with a subsystem table, and a category collision radius. `builtin:ground-vehicle`,
// `builtin:naval-vessel`, and `builtin:static-target` are passive; `builtin:sam-site` (an emitting
// `builtin:sam-radar` + a SARH launcher) and `builtin:aaa` (a cannon) shoot back when driven by the
// `sam` / `aaa` AiControllerFactory behaviors. Individual factories for tests/missions:
EntityDef builtinGroundVehicleDef();
EntityDef builtinNavalVesselDef();
// The compiled-in aircraft carrier (#38): a moving vessel with a flight deck (catapult, wires,
// LSO), sailing BuiltinCarrierVesselModel — the zero-pack proof of the launch/recovery cycle.
EntityDef builtinCarrierDef();
EntityDef builtinStaticTargetDef();
EntityDef builtinSamSiteDef();
EntityDef builtinAaaDef();

// The parachute spawned when a pilot ejects (#672): an Effect-category entity so it rides the normal
// snapshot path to every client and renders the builtin Effect placeholder. Registered by fl-server,
// which points WorldBroadcaster::setParachuteType at "builtin:parachute".
EntityDef builtinParachuteDef();

// Registers all five builtin surface entities into `registry` (the "builtin:" namespace cannot
// collide with a pack). Main thread, before GameLoop::start(). Returns the number registered.
uint32_t registerBuiltinSurfaceEntities(EntityTypeRegistry& registry);

// Builds the resolver WorldBroadcaster calls on the spawn path to turn an EntityDef::sensorIds entry
// into a parsed SensorDef (#685), routed through ContentIndex (#810).
//
// A sensor reference is an ID ("fl-base:apq159"), not an asset name. Handing it straight to
// AssetManager -- which is what this code did until #810 -- builds "sensors/fl-base:apq159.toml", a
// path that cannot exist, so EVERY aircraft in every pack silently flew with no radar. The index is
// what makes the id resolvable; this factory is what makes that resolution testable.
//
// A miss is logged at ERROR and yields nullptr: the entity keeps the rest of its suite (and, with
// none left, the builtin eyeball) rather than being denied a spawn over one missing file. Misses are
// cached too, so a bad id is not re-reported on every spawn.
//
// `assets`, `index` and `log` must outlive the returned resolver.
using SensorDefResolver = std::function<std::shared_ptr<const sensor::SensorDef>(const std::string& id)>;
SensorDefResolver makeSensorDefResolver(AssetManager& assets, const ContentIndex& index, ILogger& log);

} // namespace fl
