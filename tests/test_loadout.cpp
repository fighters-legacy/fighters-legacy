// SPDX-License-Identifier: GPL-3.0-or-later
//
// WeaponRegistry + defaultPayload (#812).
//
// Before this, EntityDef::hardpoints parsed and was read by nobody, and PayloadEffect was hard-coded
// to {} at both integrator call sites. A loadout cost the airframe zero mass and zero drag: you
// could hang six bombs off a jet and it would fly exactly as fast as a clean one.

#include "mock_log.h"
#include <ILogger.h>
#include <entity/EntityDef.h>
#include <weapon/Loadout.h>
#include <weapon/WeaponDef.h>
#include <weapon/WeaponRegistry.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>
#include <vector>

using Catch::Approx;
using namespace fl;

namespace {

WeaponDef weapon(const std::string& id, WeaponType type, float massKg, float dragFactor) {
    WeaponDef w;
    w.id = id;
    w.name = id;
    w.type = type;
    w.load.massKg = massKg;
    w.load.dragFactor = dragFactor;
    return w;
}

Hardpoint station(int slot, std::vector<std::string> allowed, std::string def) {
    Hardpoint hp;
    hp.slot = slot;
    hp.allowed = std::move(allowed);
    hp.defaultWeapon = std::move(def);
    return hp;
}

// An F-5E-shaped airframe: two wingtip AIM-9s, a centreline fuel tank, an internal gun.
EntityDef f5e() {
    EntityDef def;
    def.id = "fl-base:f5e";
    def.name = "F-5E Tiger II";
    def.hardpoints = {
        station(1, {"fl-base:aim9p"}, "fl-base:aim9p"),
        station(2, {"fl-base:aim9p"}, "fl-base:aim9p"),
        station(3, {"fl-base:tank150"}, "fl-base:tank150"),
        station(4, {"fl-base:m39a2"}, "fl-base:m39a2"),
    };
    return def;
}

WeaponRegistry f5eWeapons() {
    WeaponRegistry reg;
    reg.registerWeapon(weapon("fl-base:aim9p", WeaponType::Missile, 85.5f, 0.0012f));
    reg.registerWeapon(weapon("fl-base:m39a2", WeaponType::Gun, 102.0f, 0.f));       // internal: no drag
    reg.registerWeapon(weapon("fl-base:tank150", WeaponType::Fuel, 320.0f, 0.003f)); // drop tank (#862)
    return reg;
}

} // namespace

// ---------------------------------------------------------------------------
// WeaponRegistry
// ---------------------------------------------------------------------------

TEST_CASE("WeaponRegistry: lookup by id", "[weapon_registry]") {
    WeaponRegistry reg;
    const uint32_t idx = reg.registerWeapon(weapon("fl-base:aim9p", WeaponType::Missile, 85.5f, 0.0012f));

    REQUIRE(idx == 0u);
    REQUIRE(reg.weaponCount() == 1u);

    const WeaponDef* w = reg.findById("fl-base:aim9p");
    REQUIRE(w != nullptr);
    CHECK(w->load.massKg == Approx(85.5f));
    CHECK(reg.indexById("fl-base:aim9p") == 0u);
    CHECK(reg.byIndex(0u) == w);
}

TEST_CASE("WeaponRegistry: unknown id and out-of-range index return null", "[weapon_registry]") {
    WeaponRegistry reg;
    reg.registerWeapon(weapon("fl-base:aim9p", WeaponType::Missile, 85.5f, 0.0012f));

    CHECK(reg.findById("fl-base:nope") == nullptr);
    CHECK(reg.indexById("fl-base:nope") == std::numeric_limits<uint32_t>::max());
    CHECK(reg.byIndex(7u) == nullptr);
}

TEST_CASE("WeaponRegistry: a duplicate id is rejected", "[weapon_registry]") {
    WeaponRegistry reg;
    reg.registerWeapon(weapon("fl-base:aim9p", WeaponType::Missile, 85.5f, 0.0012f));
    const uint32_t dup = reg.registerWeapon(weapon("fl-base:aim9p", WeaponType::Missile, 999.f, 9.f));

    CHECK(dup == std::numeric_limits<uint32_t>::max());
    CHECK(reg.weaponCount() == 1u);
    CHECK(reg.findById("fl-base:aim9p")->load.massKg == Approx(85.5f)); // the first one survives
}

TEST_CASE("WeaponRegistry: clear empties it", "[weapon_registry]") {
    WeaponRegistry reg;
    reg.registerWeapon(weapon("fl-base:aim9p", WeaponType::Missile, 85.5f, 0.0012f));
    reg.clear();

    CHECK(reg.weaponCount() == 0u);
    CHECK(reg.findById("fl-base:aim9p") == nullptr);
}

// ---------------------------------------------------------------------------
// defaultPayload
// ---------------------------------------------------------------------------

TEST_CASE("defaultPayload sums an F-5E default loadout", "[loadout]") {
    RecordingLogger log;
    const WeaponRegistry reg = f5eWeapons();

    const PayloadEffect p = defaultPayload(f5e(), reg, log);

    // 2 x AIM-9P (85.5 kg, 0.0012 cd0) + gun (102 kg, no drag) + drop tank (320 kg, 0.003 cd0). The
    // fuel station carries an inert tank store (#862) whose mass/drag DOES count against the airframe.
    CHECK(p.extra_mass_kg == Approx(85.5f * 2 + 102.0f + 320.0f));
    CHECK(p.extra_cd0 == Approx(0.0012f * 2 + 0.003f));

    // Every store resolves: no log line at all.
    CHECK(log.count(LogLevel::Error) == 0);
    CHECK(log.count(LogLevel::Warn) == 0);
}

TEST_CASE("defaultPayload counts a Fuel drop-tank store (#862)", "[loadout]") {
    // A drop tank is a WeaponType::Fuel store now: inert (never fires) but it costs the airframe
    // mass + drag like any other store, so a tanked jet flies heavier than a clean one.
    RecordingLogger log;
    WeaponRegistry reg;
    reg.registerWeapon(weapon("fl-base:tank150", WeaponType::Fuel, 320.0f, 0.003f));

    EntityDef def;
    def.id = "fl-base:jet";
    def.hardpoints = {station(1, {"fl-base:tank150"}, "fl-base:tank150")};

    const PayloadEffect p = defaultPayload(def, reg, log);

    CHECK(p.extra_mass_kg == Approx(320.0f));
    CHECK(p.extra_cd0 == Approx(0.003f));
    CHECK(log.count(LogLevel::Error) == 0); // resolves cleanly
}

TEST_CASE("defaultPayload: an unknown store id yields a clean airframe and ONE Error", "[loadout]") {
    RecordingLogger log;
    WeaponRegistry reg; // empty: nobody has ever heard of this missile

    EntityDef def;
    def.id = "fl-base:jet";
    def.hardpoints = {station(1, {"fl-base:typo"}, "fl-base:typo")};

    const PayloadEffect p = defaultPayload(def, reg, log);

    // The aircraft still flies. It just flies clean, and the log says which pylon lied. Refusing to
    // spawn it over one typo would be the worse failure.
    CHECK(p.extra_mass_kg == Approx(0.f));
    CHECK(p.extra_cd0 == Approx(0.f));
    CHECK(log.count(LogLevel::Error) == 1);
    CHECK(log.hasMessage(LogLevel::Error, "fl-base:typo"));
}

TEST_CASE("defaultPayload rejects a default that is not in the station's allowed list", "[loadout]") {
    RecordingLogger log;
    WeaponRegistry reg = f5eWeapons();
    reg.registerWeapon(weapon("fl-base:aim120", WeaponType::Missile, 152.f, 0.002f));

    EntityDef def;
    def.id = "fl-base:jet";
    // The store exists, but this airframe does not clear it -- honouring it silently would let a
    // content bug arm an aircraft with something it cannot carry.
    def.hardpoints = {station(1, {"fl-base:aim9p"}, "fl-base:aim120")};

    const PayloadEffect p = defaultPayload(def, reg, log);

    CHECK(p.extra_mass_kg == Approx(0.f));
    CHECK(log.hasMessage(LogLevel::Error, "allowed list"));
}

TEST_CASE("defaultPayload: a multi-role station carries any allowed kind", "[loadout]") {
    // Stations have no kind of their own -- the allowed list is the whole compatibility contract.
    // A wet pylon that clears a bomb AND a drop tank is a real airframe (an F-16's stations 4/6),
    // and whichever allowed store is the default, its mass and drag count the same way.
    RecordingLogger log;
    WeaponRegistry reg;
    reg.registerWeapon(weapon("fl-base:mk82", WeaponType::Bomb, 227.f, 0.004f));
    reg.registerWeapon(weapon("fl-base:tank370", WeaponType::Fuel, 1160.f, 0.006f));

    EntityDef def;
    def.id = "fl-base:jet";
    def.hardpoints = {station(1, {"fl-base:mk82", "fl-base:tank370"}, "fl-base:mk82"),
                      station(2, {"fl-base:mk82", "fl-base:tank370"}, "fl-base:tank370")};

    const PayloadEffect p = defaultPayload(def, reg, log);

    CHECK(p.extra_mass_kg == Approx(227.f + 1160.f));
    CHECK(p.extra_cd0 == Approx(0.004f + 0.006f));
    CHECK(log.count(LogLevel::Error) == 0);
}

TEST_CASE("defaultPayload: an empty station is a legitimate loadout", "[loadout]") {
    RecordingLogger log;
    const WeaponRegistry reg = f5eWeapons();

    EntityDef def;
    def.id = "fl-base:jet";
    def.hardpoints = {station(1, {"fl-base:aim9p"}, "")};

    const PayloadEffect p = defaultPayload(def, reg, log);

    CHECK(p.extra_mass_kg == Approx(0.f));
    CHECK(log.count(LogLevel::Error) == 0);
}

TEST_CASE("defaultPayload: no hardpoints at all is a clean airframe", "[loadout]") {
    RecordingLogger log;
    const WeaponRegistry reg = f5eWeapons();

    EntityDef def;
    def.id = "builtin:debug-entity";

    const PayloadEffect p = defaultPayload(def, reg, log);

    CHECK(p.extra_mass_kg == Approx(0.f));
    CHECK(p.extra_cd0 == Approx(0.f));
    CHECK(log.count(LogLevel::Error) == 0);
}
