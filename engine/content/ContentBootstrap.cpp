// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/ContentBootstrap.h"

#include "ILogger.h"
#include "content/AssetManager.h"
#include "content/ContentIndex.h"
#include "entity/EntityDef.h"
#include "entity/EntityDefParser.h"
#include "entity/EntityTypeRegistry.h"
#include "sensor/BuiltinSensors.h"
#include "sensor/SensorDef.h"
#include "sensor/SensorDefParser.h"
#include "weapon/BuiltinWeapon.h"    // the sandbox loadout (#440)
#include "weapon/ProjectileSystem.h" // projectileTypeId (#625)
#include "weapon/WeaponDef.h"
#include "weapon/WeaponDefParser.h"
#include "weapon/WeaponRegistry.h"
#include "world/AirportDef.h"
#include "world/AirportDefParser.h"
#include "world/EscalationPolicy.h"
#include "world/EscalationPolicyParser.h"

#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>

namespace fl {

uint32_t registerPackEntityDefs(AssetManager& assets, EntityTypeRegistry& registry, ILogger& log) {
    uint32_t registered = 0;
    for (const auto& name : assets.listAssets(AssetType::EntityDef)) {
        auto raw = assets.loadEntityDef(name.c_str());
        if (!raw || raw->bytes.empty()) {
            log.log(LogLevel::Warn, __FILE__, __LINE__,
                    (std::string("entity def '") + name + "' could not be loaded; skipping").c_str());
            continue;
        }
        try {
            EntityDef def =
                parseEntityDef(std::string_view(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size()));
            const std::string id = def.id;
            if (registry.registerType(std::move(def)) == std::numeric_limits<uint32_t>::max())
                log.log(LogLevel::Warn, __FILE__, __LINE__,
                        (std::string("entity def id '") + id + "' already registered; skipping duplicate").c_str());
            else
                ++registered;
        } catch (const std::exception& e) {
            log.log(LogLevel::Warn, __FILE__, __LINE__,
                    (std::string("entity def '") + name + "' parse error: " + e.what() + "; skipping").c_str());
        }
    }
    return registered;
}

uint32_t registerPackAirportDefs(AssetManager& assets, std::vector<AirportDef>& out, ILogger& log) {
    uint32_t appended = 0;
    for (const auto& name : assets.listAssets(AssetType::Airport)) {
        auto raw = assets.loadAirportDef(name.c_str());
        if (!raw || raw->bytes.empty()) {
            log.log(LogLevel::Warn, __FILE__, __LINE__,
                    (std::string("airport def '") + name + "' could not be loaded; skipping").c_str());
            continue;
        }
        try {
            AirportDef def =
                parseAirportDef(std::string_view(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size()));
            out.push_back(std::move(def));
            ++appended;
        } catch (const std::exception& e) {
            log.log(LogLevel::Warn, __FILE__, __LINE__,
                    (std::string("airport def '") + name + "' parse error: " + e.what() + "; skipping").c_str());
        }
    }
    return appended;
}

uint32_t registerPackZonePolicies(AssetManager& assets, std::vector<EscalationPolicy>& out, ILogger& log) {
    uint32_t appended = 0;
    for (const auto& name : assets.listAssets(AssetType::ZonePolicy)) {
        auto raw = assets.loadZonePolicy(name.c_str());
        if (!raw || raw->bytes.empty()) {
            log.log(LogLevel::Warn, __FILE__, __LINE__,
                    (std::string("zone policy '") + name + "' could not be loaded; skipping").c_str());
            continue;
        }
        try {
            out.push_back(parseEscalationPolicy(
                std::string_view(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size())));
            ++appended;
        } catch (const std::exception& e) {
            log.log(LogLevel::Warn, __FILE__, __LINE__,
                    (std::string("zone policy '") + name + "' parse error: " + e.what() + "; skipping").c_str());
        }
    }
    return appended;
}

uint32_t registerPackWeaponDefs(AssetManager& assets, WeaponRegistry& registry, ILogger& log) {
    uint32_t registered = 0;
    for (const auto& name : assets.listAssets(AssetType::Weapon)) {
        auto raw = assets.loadWeaponDef(name.c_str());
        if (!raw || raw->bytes.empty()) {
            log.log(LogLevel::Warn, __FILE__, __LINE__,
                    (std::string("weapon def '") + name + "' could not be loaded; skipping").c_str());
            continue;
        }
        try {
            WeaponDef def =
                parseWeaponDef(std::string_view(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size()));
            if (def.seeker && def.seeker->usesLegacyLobe())
                log.log(LogLevel::Warn, __FILE__, __LINE__,
                        (std::string("weapon def '") + name +
                         "' uses the deprecated [seeker] fov_deg/acquisition_nm lobe — reference a "
                         "sensor def with sensor_id (2026-07-14 decision record); the legacy form is "
                         "removed after one release")
                            .c_str());
            const std::string id = def.id;
            if (registry.registerWeapon(std::move(def)) == std::numeric_limits<uint32_t>::max())
                log.log(LogLevel::Warn, __FILE__, __LINE__,
                        (std::string("weapon def id '") + id + "' already registered; skipping duplicate").c_str());
            else
                ++registered;
        } catch (const std::exception& e) {
            log.log(LogLevel::Warn, __FILE__, __LINE__,
                    (std::string("weapon def '") + name + "' parse error: " + e.what() + "; skipping").c_str());
        }
    }
    return registered;
}

SensorDefResolver makeSensorDefResolver(AssetManager& assets, const ContentIndex& index, ILogger& log) {
    // Shared so the cache survives the copy into WorldBroadcaster's std::function.
    auto cache = std::make_shared<std::unordered_map<std::string, std::shared_ptr<const sensor::SensorDef>>>();

    return [&assets, &index, &log, cache](const std::string& id) -> std::shared_ptr<const sensor::SensorDef> {
        if (auto it = cache->find(id); it != cache->end())
            return it->second;

        // Builtin ids resolve to the compiled-in defs — no pack, no file, no error (#440/#627).
        // Checked BEFORE the index, deliberately: "builtin:" is engine vocabulary, and a pack that
        // shadowed it could make the sandbox seeker mean something different per server. Non-owning
        // pointers via the aliasing constructor — the statics outlive everything.
        for (const sensor::SensorDef* builtin :
             {&sensor::BuiltinSensors::eyeball(), &sensor::BuiltinSensors::irSeeker(),
              &sensor::BuiltinSensors::radarSeeker(), &sensor::BuiltinSensors::sarhSeeker(),
              &sensor::BuiltinSensors::groundRadar()}) {
            if (id == builtin->id) {
                std::shared_ptr<const sensor::SensorDef> def(std::shared_ptr<const sensor::SensorDef>{}, builtin);
                (*cache)[id] = def;
                return def;
            }
        }

        std::shared_ptr<const sensor::SensorDef> def;
        const std::string* assetName = index.assetNameFor(AssetType::SensorDef, id);

        if (!assetName) {
            log.log(LogLevel::Error, __FILE__, __LINE__,
                    ("unknown sensor def id '" + id +
                     "': no loaded pack declares it -- the entity will fly without this sensor")
                        .c_str());
        } else if (auto raw = assets.loadSensorDef(assetName->c_str()); raw && !raw->bytes.empty()) {
            try {
                def = std::make_shared<const sensor::SensorDef>(sensor::parseSensorDef(
                    std::string_view(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size())));
            } catch (const std::exception& e) {
                log.log(LogLevel::Error, __FILE__, __LINE__,
                        ("sensor def '" + id + "' (asset '" + *assetName + "') failed to parse: " + e.what()).c_str());
            }
        } else {
            log.log(LogLevel::Error, __FILE__, __LINE__,
                    ("sensor def '" + id + "' is indexed as asset '" + *assetName + "' but the asset failed to load")
                        .c_str());
        }

        (*cache)[id] = def; // cache misses too, so a bad id isn't re-reported on every spawn
        return def;
    };
}

uint32_t registerProjectileEntityDefs(const WeaponRegistry& weapons, EntityTypeRegistry& registry, ILogger& log) {
    uint32_t registered = 0;
    for (uint32_t i = 0;; ++i) {
        const WeaponDef* w = weapons.byIndex(i);
        if (!w)
            break;
        if (w->type == WeaponType::Gun || w->type == WeaponType::Pod || w->type == WeaponType::Fuel)
            continue; // hitscan / non-flying / inert stores never become entities

        EntityDef def;
        def.id = projectileTypeId(*w);
        def.name = w->name;
        def.category = ObjectCategory::Projectile;
        // Explicit switch, never an ordinal cast: WeaponType and ProjectileKind are parallel
        // vocabularies with different ordinals -- an ordinal cast is a silent kind-swap trap.
        switch (w->type) {
        case WeaponType::Missile:
            def.projectileKind = ProjectileKind::Missile;
            break;
        case WeaponType::Bomb:
            def.projectileKind = ProjectileKind::Bomb;
            break;
        case WeaponType::Rocket:
            def.projectileKind = ProjectileKind::Rocket;
            break;
        case WeaponType::Gun:
        case WeaponType::Pod:
        case WeaponType::Fuel:
            break; // unreachable — skipped above; listed so -Wswitch stays exhaustive
        }
        def.maxHp = 1.f;
        def.mesh = w->mesh; // ASSET NAME; empty = the builtin placeholder
        // A missile is a hard radar target to SEE: small RCS, hot IR while the motor burns (the
        // static signature approximates the burn — per-phase signatures can come with #529).
        def.signatures.rcs = 0.1f;
        def.signatures.visual = 0.3f;
        def.signatures.ir = 2.0f;

        if (registry.registerType(std::move(def)) == std::numeric_limits<uint32_t>::max()) {
            log.log(LogLevel::Warn, __FILE__, __LINE__,
                    (std::string("projectile type for weapon '") + w->id + "' already registered; skipping").c_str());
        } else {
            ++registered;
        }
    }
    return registered;
}

uint32_t registerBuiltinWeapons(WeaponRegistry& registry) {
    uint32_t registered = 0;
    for (const WeaponDef* w : {&BuiltinWeapon::cannon(), &BuiltinWeapon::irMissile(), &BuiltinWeapon::radarMissile(),
                               &BuiltinWeapon::sarhMissile(), &BuiltinWeapon::bomb(), &BuiltinWeapon::rocketPod(),
                               &BuiltinWeapon::dropTank(), &BuiltinWeapon::pod()}) {
        if (registry.registerWeapon(*w) != std::numeric_limits<uint32_t>::max())
            ++registered;
    }
    return registered;
}

EntityDef builtinDebugEntityDef() {
    EntityDef def;
    def.id = "builtin:debug-entity";
    def.name = "Debug Entity";
    def.category = ObjectCategory::AirVehicle;
    def.maxHp = 100.0f;

    // A real 3-level damage model + the full aircraft subsystem table (#864), so progressive damage,
    // asymmetric-thrust / control degradation, avionics loss and the damage-variant mesh swap all run
    // with zero content mounted — the debug entity had binary death before. The `visualEffect` names
    // resolve to the builtin particle presets (registered by the game client).
    {
        DamageDef d;
        d.light.hpFraction = 0.66f;
        d.light.visualEffect = "smoke";
        d.heavy.hpFraction = 0.33f;
        d.heavy.visualEffect = "fire";
        d.heavy.thrustFactor = 0.7f;
        d.heavy.controlFactor = 0.6f;
        d.critical.hpFraction = 0.12f;
        d.critical.visualEffect = "fire";
        d.critical.thrustFactor = 0.4f;
        d.critical.controlFactor = 0.3f;
        d.critical.avionicsFailure = true;

        SubsystemSet subs;
        subs.parts[static_cast<int>(Subsystem::EngineLeft)] = {30.f, 1.0f}; // asymmetric thrust on loss
        subs.parts[static_cast<int>(Subsystem::EngineRight)] = {30.f, 1.0f};
        subs.parts[static_cast<int>(Subsystem::Controls)] = {40.f, 1.0f};
        subs.parts[static_cast<int>(Subsystem::Avionics)] = {25.f, 0.8f};
        subs.parts[static_cast<int>(Subsystem::Hydraulics)] = {30.f, 0.8f};
        subs.parts[static_cast<int>(Subsystem::Fuel)] = {35.f, 1.0f};
        d.subsystems = subs;
        def.damage = d;
    }

    // Armed (#440/#862): a cannon, IR + radar + SARH rails, a bomb, a rocket pod, a drop tank, and a
    // sensor pod — every sandbox/debug peer spawns able to exercise the WHOLE fire path (every
    // WeaponType, incl. the inert Fuel/Pod stores) with zero content mounted. Stations have no
    // kind of their own; each one's allowed list is its whole compatibility contract.
    auto hp = [](int slot, const char* weapon) {
        Hardpoint h;
        h.slot = slot;
        h.allowed = {weapon};
        h.defaultWeapon = weapon;
        return h;
    };
    def.hardpoints = {
        hp(0, BuiltinWeapon::cannon().id.c_str()),       hp(1, BuiltinWeapon::irMissile().id.c_str()),
        hp(2, BuiltinWeapon::radarMissile().id.c_str()), hp(3, BuiltinWeapon::sarhMissile().id.c_str()),
        hp(4, BuiltinWeapon::bomb().id.c_str()),         hp(5, BuiltinWeapon::rocketPod().id.c_str()),
        hp(6, BuiltinWeapon::dropTank().id.c_str()),     hp(7, BuiltinWeapon::pod().id.c_str()),
    };
    return def;
}

EntityDef builtinBomberDef() {
    // The compiled-in MULTI-CREW aircraft (#966/#977): a Fly+Fire+Radar+Countermeasures pilot dropping
    // bombs off station 0, and a defensive TAIL-GUNNER seat aiming a rear-facing cannon turret on
    // station 1, filled by the builtin turret gunner (#971). It makes the whole crew → seat-sampling →
    // turret-slew → directional-fire path provable with ZERO content packs, per the armed-sandbox
    // doctrine — the crewed counterpart to builtin:debug-entity.
    EntityDef def;
    def.id = "builtin:bomber";
    def.name = "Heavy Bomber";
    def.category = ObjectCategory::AirVehicle;
    def.maxHp = 600.0f; // a bomber soaks damage; also gives the tail gun something to defend
    def.collisionRadiusM = 14.f;
    def.signatures.rcs = 4.f; // big and slow: easy to see, easy to lock — the tail gun is its answer
    def.signatures.ir = 2.f;
    def.signatures.visual = 2.f;

    {
        DamageDef d;
        d.light.hpFraction = 0.66f;
        d.light.visualEffect = "smoke";
        d.heavy.hpFraction = 0.33f;
        d.heavy.visualEffect = "fire";
        d.heavy.thrustFactor = 0.75f;
        d.heavy.controlFactor = 0.7f;
        d.critical.hpFraction = 0.12f;
        d.critical.visualEffect = "fire";
        d.critical.thrustFactor = 0.4f;
        d.critical.controlFactor = 0.35f;
        d.critical.avionicsFailure = true;

        SubsystemSet subs;
        subs.parts[static_cast<int>(Subsystem::EngineLeft)] = {40.f, 1.0f};
        subs.parts[static_cast<int>(Subsystem::EngineRight)] = {40.f, 1.0f};
        subs.parts[static_cast<int>(Subsystem::Controls)] = {50.f, 1.0f};
        subs.parts[static_cast<int>(Subsystem::Avionics)] = {30.f, 0.8f};
        subs.parts[static_cast<int>(Subsystem::Fuel)] = {45.f, 1.0f};
        d.subsystems = subs;
        def.damage = d;
    }

    auto hp = [](int slot, const char* weapon) {
        Hardpoint h;
        h.slot = slot;
        h.allowed = {weapon};
        h.defaultWeapon = weapon;
        return h;
    };
    def.hardpoints = {
        hp(0, BuiltinWeapon::bomb().id.c_str()),   // the pilot's bomb bay
        hp(1, BuiltinWeapon::cannon().id.c_str()), // the tail gun, mounted on the turret below
    };

    // The defensive tail turret: mounted facing AFT (a 180-deg yaw about +Y maps the bore to -X), with
    // a realistic rear-quarter traverse envelope and a modest slew rate. Its station (1) is fired by
    // the gunner seat.
    TurretDef tail;
    tail.id = "tail";
    tail.mountPos[0] = 0.f;
    tail.mountPos[1] = 0.6f;
    tail.mountPos[2] = -8.f;
    tail.mountOrient[0] = 0.f; // quat (x,y,z,w) for a 180-deg rotation about +Y = (0,1,0,0)
    tail.mountOrient[1] = 1.f;
    tail.mountOrient[2] = 0.f;
    tail.mountOrient[3] = 0.f;
    tail.azMinDeg = -80.f;
    tail.azMaxDeg = 80.f;
    tail.elMinDeg = -10.f;
    tail.elMaxDeg = 80.f;
    tail.slewRateDegS = 70.f;
    tail.stations = {1};
    def.turrets = {tail};

    SeatDef pilot;
    pilot.role = "pilot";
    pilot.capabilities = withCapability(
        withCapability(withCapability(withCapability(CrewCapabilityMask{0}, CrewCapability::Fly), CrewCapability::Fire),
                       CrewCapability::Radar),
        CrewCapability::Countermeasures);
    pilot.stations = {0};
    pilot.eyepoint[1] = 1.4f;
    pilot.eyepoint[2] = 6.0f;

    SeatDef gunner;
    gunner.role = "tail-gunner";
    gunner.capabilities = withCapability(CrewCapabilityMask{0}, CrewCapability::Fire);
    gunner.turret = "tail";
    gunner.botSpec = "builtin:gunner";
    gunner.defaultSkill = 0.6f;
    gunner.eyepoint[1] = 0.6f;
    gunner.eyepoint[2] = -8.0f;

    def.crew = {pilot, gunner};
    return def;
}

namespace {

// A shared 3-level DamageDef + subsystem table for the builtin surface entities (#863). The fixed
// subsystem vocabulary (#675) is aircraft-shaped (engine_left/right, controls, ...), so for a surface
// unit only the subsystems that MEAN something on the ground are modeled (hp > 0): avionics (the
// sensor/fire-control gear), controls (the traverse/drive gear), and fuel (a fire/ammo cook-off
// pool). The engine pools stay hp = 0 — a bunker has no left engine to shoot out.
DamageDef builtinSurfaceDamageDef() {
    DamageDef d;
    d.light.hpFraction = 0.66f;
    d.light.visualEffect = "smoke";
    d.heavy.hpFraction = 0.33f;
    d.heavy.visualEffect = "fire";
    d.heavy.controlFactor = 0.5f;
    d.critical.hpFraction = 0.12f;
    d.critical.visualEffect = "fire";
    d.critical.controlFactor = 0.2f;
    d.critical.avionicsFailure = true;

    SubsystemSet subs;
    subs.parts[static_cast<int>(Subsystem::Avionics)] = {40.f, 1.5f}; // sensors / fire control
    subs.parts[static_cast<int>(Subsystem::Controls)] = {40.f, 1.0f}; // traverse / drive gear
    subs.parts[static_cast<int>(Subsystem::Fuel)] = {30.f, 1.0f};     // fuel / ammo cook-off
    d.subsystems = subs;
    return d;
}

// Fill the common fields every builtin surface unit shares: category, damage model, signature, and an
// explicit collision radius. Passive targets stop here; the shooters add sensors + a hardpoint.
EntityDef makeSurfaceDef(const char* id, const char* name, ObjectCategory cat, float maxHp, float rcs, float ir,
                         float visual, float collisionRadiusM) {
    EntityDef def;
    def.id = id;
    def.name = name;
    def.category = cat;
    def.maxHp = maxHp;
    def.damage = builtinSurfaceDamageDef();
    def.signatures.rcs = rcs;
    def.signatures.ir = ir;
    def.signatures.visual = visual;
    def.collisionRadiusM = collisionRadiusM;
    return def;
}

} // namespace

EntityDef builtinGroundVehicleDef() {
    // A passive ground target — bigger and easier to see than a fighter, but no threat of its own.
    return makeSurfaceDef("builtin:ground-vehicle", "Ground Vehicle", ObjectCategory::GroundVehicle, 200.f,
                          /*rcs=*/5.f, /*ir=*/3.f, /*visual=*/2.f, /*collision=*/8.f);
}

EntityDef builtinNavalVesselDef() {
    // A passive naval target — a big radar and visual return, a lot of hull to chew through.
    return makeSurfaceDef("builtin:naval-vessel", "Naval Vessel", ObjectCategory::NavalVehicle, 4000.f,
                          /*rcs=*/60.f, /*ir=*/8.f, /*visual=*/12.f, /*collision=*/45.f);
}
EntityDef builtinCarrierDef() {
    // The compiled-in aircraft carrier (#38): a MOVING vessel with a flight deck, so the whole
    // launch/recovery cycle — waypoint steaming, deck landings, catapult shots, arrested traps,
    // LSO calls — is provable with ZERO content packs (the armed-sandbox doctrine). Sails on
    // BuiltinCarrierVesselModel via the flightModelAsset name below.
    EntityDef def = makeSurfaceDef("builtin:carrier", "Aircraft Carrier", ObjectCategory::NavalVehicle, 8000.f,
                                   /*rcs=*/200.f, /*ir=*/15.f, /*visual=*/30.f, /*collision=*/170.f);
    def.flightModelAsset = "builtin:carrier-vessel"; // resolved by fl-server's flight-model resolver
    def.acceptsLandings = true;                      // the #699 seam, finally consumed
    DeckDef deck;                                    // Nimitz-ish numbers, engine defaults
    deck.lengthM = 330.f;
    deck.widthM = 75.f;
    deck.heightM = 20.f;
    deck.catStartXM = 30.f;
    deck.catStrokeM = 100.f;
    deck.catEndSpeedMps = 75.f;
    deck.wireXM = -110.f;
    deck.wireZoneM = 40.f;
    deck.maxTrapSpeedMps = 80.f;
    def.deck = deck;
    return def;
}

EntityDef builtinStaticTargetDef() {
    // A fixed structure — a bunker/hangar-class ground target for strike practice.
    return makeSurfaceDef("builtin:static-target", "Static Structure", ObjectCategory::Structure, 800.f,
                          /*rcs=*/15.f, /*ir=*/1.f, /*visual=*/6.f, /*collision=*/20.f);
}

EntityDef builtinSamSiteDef() {
    // An emitting ground radar (builtin:sam-radar — the RWR seam) + a SARH launcher. Driven by the
    // `sam` AiControllerFactory behavior, it acquires an aircraft on radar and launches.
    EntityDef def = makeSurfaceDef("builtin:sam-site", "SAM Site", ObjectCategory::GroundVehicle, 150.f,
                                   /*rcs=*/3.f, /*ir=*/2.f, /*visual=*/2.f, /*collision=*/10.f);
    def.sensorIds = {"builtin:sam-radar"};
    Hardpoint launcher;
    launcher.slot = 0;
    launcher.allowed = {BuiltinWeapon::sarhMissile().id};
    launcher.defaultWeapon = BuiltinWeapon::sarhMissile().id;
    def.hardpoints = {launcher};
    return def;
}

EntityDef builtinAaaDef() {
    // A gun-based air-defense emplacement. Driven by the `aaa` behavior, it leads an aircraft with the
    // ballistic solution and fires when the target crosses its engagement cone. Senses on the builtin
    // eyeball (no declared sensors) — honest, short-legged optical tracking.
    EntityDef def = makeSurfaceDef("builtin:aaa", "AAA Emplacement", ObjectCategory::GroundVehicle, 120.f,
                                   /*rcs=*/2.f, /*ir=*/2.f, /*visual=*/2.f, /*collision=*/8.f);
    Hardpoint gun;
    gun.slot = 0;
    gun.allowed = {BuiltinWeapon::cannon().id};
    gun.defaultWeapon = BuiltinWeapon::cannon().id;
    def.hardpoints = {gun};
    return def;
}

EntityDef builtinParachuteDef() {
    // The ejection parachute (#672): an Effect-category entity — a small, cosmetic, low-signature object
    // that rides the snapshot path so every client sees a chute where a pilot got out. Not a threat and
    // not a strike target; it renders the builtin Effect placeholder.
    EntityDef def;
    def.id = "builtin:parachute";
    def.name = "Parachute";
    def.category = ObjectCategory::Effect;
    def.maxHp = 1.f;
    def.signatures.rcs = 0.2f;
    def.signatures.ir = 0.2f;
    def.signatures.visual = 3.f;
    def.collisionRadiusM = 2.f;
    return def;
}

uint32_t registerBuiltinSurfaceEntities(EntityTypeRegistry& registry) {
    uint32_t registered = 0;
    for (const EntityDef& def : {builtinGroundVehicleDef(), builtinNavalVesselDef(), builtinStaticTargetDef(),
                                 builtinSamSiteDef(), builtinAaaDef(), builtinCarrierDef()}) {
        if (registry.registerType(EntityDef(def)) != std::numeric_limits<uint32_t>::max())
            ++registered;
    }
    return registered;
}

} // namespace fl
