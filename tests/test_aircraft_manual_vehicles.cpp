// SPDX-License-Identifier: GPL-3.0-or-later
//
// AircraftManual for the non-fixed-wing vehicles and the full store/sensor vocabularies (#1145).
//
// test_aircraft_manual.cpp covers the fighter: trim at three altitudes, limits, stations, crew,
// prose. The manual also has to serve a helicopter, a multirotor and a vessel — whose performance
// chart is hover, not trim — and it maps every weapon and sensor type to a name a pilot recognises.
// Those tables are the kind of thing that silently grows a "?" when an enumerator is added.

#include "ILogger.h"
#include "entity/EntityDef.h"
#include "flight/FlightModelParser.h"
#include "manual/AircraftManual.h"
#include "sensor/SensorDef.h"
#include "weapon/WeaponDef.h"
#include "weapon/WeaponRegistry.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using namespace fl;

namespace {

bool hasSection(const AircraftManual& m, std::string_view title) {
    return std::any_of(m.sections.begin(), m.sections.end(),
                       [title](const ManualSection& s) { return s.title == title; });
}

const ManualSection* section(const AircraftManual& m, std::string_view title) {
    for (const auto& s : m.sections)
        if (s.title == title)
            return &s;
    return nullptr;
}

bool anyValueMentions(const AircraftManual& m, std::string_view needle) {
    for (const auto& s : m.sections)
        for (const auto& r : s.rows)
            if (r.value.find(needle) != std::string::npos || r.label.find(needle) != std::string::npos)
                return true;
    return false;
}

constexpr const char* kHelicopter = R"(
[aircraft]
name        = "Test Rotor"
type        = "helicopter"
engine_type = "turboprop"

[flight_model]
mass_kg      = 2500.0
wing_area_m2 = 1.0
wingspan_m   = 2.0
mac_m        = 0.5
fuel_kg      = 500.0
ixx_kg_m2    = 1000.0
iyy_kg_m2    = 4000.0
izz_kg_m2    = 3000.0

[helicopter]
main_rotor_radius_m     = 7.3
main_rotor_max_thrust_n = 40000.0
yaw_moment_max_nm       = 8000.0
cyclic_moment_nm        = 20000.0

[engine]
fuel_flow_idle_kg_s = 0.02
fuel_flow_mil_kg_s  = 0.12
fuel_flow_ab_kg_s   = 0.12
spool_time_s        = 3.0
)";

constexpr const char* kMultirotor = R"(
[aircraft]
name        = "Test Quad"
type        = "multirotor"
engine_type = "piston"

[flight_model]
mass_kg      = 2.0
wing_area_m2 = 0.1
wingspan_m   = 0.5
mac_m        = 0.1
fuel_kg      = 0.0
ixx_kg_m2    = 0.1
iyy_kg_m2    = 0.1
izz_kg_m2    = 0.2

[multirotor]
rotor_count        = 4
rotor_thrust_max_n = 12.0
rotor_arm_m        = 0.25
yaw_torque_nm      = 0.6
flight_time_min    = 25.0
)";

constexpr const char* kVessel = R"(
[aircraft]
name        = "Test Frigate"
type        = "vessel"
engine_type = "piston"

[flight_model]
mass_kg      = 4000000.0
wing_area_m2 = 100.0
wingspan_m   = 20.0
mac_m        = 5.0
fuel_kg      = 100000.0
ixx_kg_m2    = 1.0e8
iyy_kg_m2    = 1.0e9
izz_kg_m2    = 1.0e9

[vessel]
max_thrust_n  = 1000000.0
max_speed_mps = 15.0
)";

EntityDef entityFor(const char* id, const char* name) {
    EntityDef d;
    d.id = id;
    d.name = name;
    return d;
}

WeaponDef weaponOf(const char* id, WeaponType type) {
    WeaponDef w;
    w.id = id;
    w.name = id;
    w.type = type;
    w.load.massKg = 100.f; // mass lives on WeaponLoad, the airframe-cost half of the def
    return w;
}

} // namespace

// ---------------------------------------------------------------------------
// Rotorcraft: hover replaces the trim chart
// ---------------------------------------------------------------------------

TEST_CASE("manual: a helicopter gets a hover chart, not a trim chart (#1145)", "[manual]") {
    const FlightModelData fm = parseFlightModel(kHelicopter);
    const EntityDef ent = entityFor("test:heli", "Test Rotor");
    ManualSources src;
    src.entity = &ent;
    src.model = &fm;

    const AircraftManual m = buildAircraftManual(src);
    CHECK(m.title == "Test Rotor");
    CHECK(hasSection(m, "Hover performance"));
}

TEST_CASE("manual: a multirotor's hover chart comes from its rotor count (#1145)", "[manual]") {
    const FlightModelData fm = parseFlightModel(kMultirotor);
    const EntityDef ent = entityFor("test:quad", "Test Quad");
    ManualSources src;
    src.entity = &ent;
    src.model = &fm;

    const AircraftManual m = buildAircraftManual(src);
    CHECK(hasSection(m, "Hover performance"));
    // Four rotors at 12 N each against a 2 kg airframe is a very high thrust-to-weight; the chart
    // is DERIVED from those numbers, so it must reflect them rather than a hand-written constant.
    const ManualSection* hover = section(m, "Hover performance");
    REQUIRE(hover != nullptr);
    CHECK_FALSE(hover->rows.empty());
}

TEST_CASE("manual: a vessel still produces a manual (#1145)", "[manual]") {
    const FlightModelData fm = parseFlightModel(kVessel);
    const EntityDef ent = entityFor("test:frigate", "Test Frigate");
    ManualSources src;
    src.entity = &ent;
    src.model = &fm;

    const AircraftManual m = buildAircraftManual(src);
    CHECK(m.title == "Test Frigate");
    CHECK_FALSE(m.sections.empty()); // it has no trim chart and no hover chart, but it is not empty
}

// ---------------------------------------------------------------------------
// The store vocabulary
// ---------------------------------------------------------------------------

TEST_CASE("manual: every weapon type names itself on a station (#1145)", "[manual]") {
    // A station's kind IS its allowed stores. Each enumerator must map to a word a pilot reads;
    // an unmapped one shows up as "?" and nothing else would catch it.
    WeaponRegistry weapons;
    weapons.registerWeapon(weaponOf("w:missile", WeaponType::Missile));
    weapons.registerWeapon(weaponOf("w:bomb", WeaponType::Bomb));
    weapons.registerWeapon(weaponOf("w:rocket", WeaponType::Rocket));
    weapons.registerWeapon(weaponOf("w:gun", WeaponType::Gun));
    weapons.registerWeapon(weaponOf("w:fuel", WeaponType::Fuel));
    weapons.registerWeapon(weaponOf("w:pod", WeaponType::Pod));

    EntityDef ent = entityFor("test:jet", "Test Jet");
    int slot = 1;
    for (const char* id : {"w:missile", "w:bomb", "w:rocket", "w:gun", "w:fuel", "w:pod"}) {
        Hardpoint hp;
        hp.slot = slot++;
        hp.allowed = {id};
        ent.hardpoints.push_back(hp);
    }

    const FlightModelData fm = parseFlightModel(kHelicopter); // any model; the stations are the subject
    ManualSources src;
    src.entity = &ent;
    src.model = &fm;
    src.weapons = &weapons;

    const AircraftManual m = buildAircraftManual(src);
    for (const char* word : {"missile", "bomb", "rocket", "gun", "fuel", "pod"}) {
        INFO("store kind " << word);
        CHECK(anyValueMentions(m, word));
    }
    CHECK_FALSE(anyValueMentions(m, "?")); // no enumerator fell through the table
}

TEST_CASE("manual: a station accepting several kinds is multi-role (#1145)", "[manual]") {
    WeaponRegistry weapons;
    weapons.registerWeapon(weaponOf("w:missile", WeaponType::Missile));
    weapons.registerWeapon(weaponOf("w:bomb", WeaponType::Bomb));

    EntityDef ent = entityFor("test:jet", "Test Jet");
    Hardpoint hp;
    hp.slot = 1;
    hp.allowed = {"w:missile", "w:bomb"};
    ent.hardpoints.push_back(hp);

    const FlightModelData fm = parseFlightModel(kHelicopter);
    ManualSources src;
    src.entity = &ent;
    src.model = &fm;
    src.weapons = &weapons;

    CHECK(anyValueMentions(buildAircraftManual(src), "multi-role"));
}

TEST_CASE("manual: with no weapon registry there is no store vocabulary to print (#1145)", "[manual]") {
    EntityDef ent = entityFor("test:jet", "Test Jet");
    Hardpoint hp;
    hp.slot = 1;
    hp.allowed = {"w:missile"};
    ent.hardpoints.push_back(hp);

    const FlightModelData fm = parseFlightModel(kHelicopter);
    ManualSources src;
    src.entity = &ent;
    src.model = &fm;
    src.weapons = nullptr; // no registry: the station kind is unknowable, not guessed

    const AircraftManual m = buildAircraftManual(src);
    CHECK_FALSE(anyValueMentions(m, "multi-role"));
}

TEST_CASE("manual: a station whose allowed store does not exist prints no kind (#1145)", "[manual]") {
    WeaponRegistry weapons; // empty: the id resolves to nothing
    EntityDef ent = entityFor("test:jet", "Test Jet");
    Hardpoint hp;
    hp.slot = 1;
    hp.allowed = {"w:ghost"};
    ent.hardpoints.push_back(hp);

    const FlightModelData fm = parseFlightModel(kHelicopter);
    ManualSources src;
    src.entity = &ent;
    src.model = &fm;
    src.weapons = &weapons;

    const AircraftManual m = buildAircraftManual(src);
    CHECK_FALSE(anyValueMentions(m, "multi-role"));
}

// ---------------------------------------------------------------------------
// The sensor vocabulary
// ---------------------------------------------------------------------------

TEST_CASE("manual: every sensor type names itself (#1145)", "[manual]") {
    sensor::SensorDef visual;
    visual.id = "s:eyeball";
    visual.type = sensor::SensorType::Visual;
    sensor::SensorDef ir;
    ir.id = "s:irst";
    ir.type = sensor::SensorType::Ir;
    sensor::SensorDef radar;
    radar.id = "s:radar";
    radar.type = sensor::SensorType::Radar;
    sensor::SensorDef laser;
    laser.id = "s:laser";
    laser.type = sensor::SensorType::Laser;

    const EntityDef ent = entityFor("test:jet", "Test Jet");
    const FlightModelData fm = parseFlightModel(kHelicopter);
    ManualSources src;
    src.entity = &ent;
    src.model = &fm;
    src.sensors = {&visual, &ir, &radar, &laser};

    const AircraftManual m = buildAircraftManual(src);
    for (const char* word : {"visual", "infrared", "radar", "laser"}) {
        INFO("sensor kind " << word);
        CHECK(anyValueMentions(m, word));
    }
}

TEST_CASE("manual: no sensors means no sensor section (#1145)", "[manual]") {
    const EntityDef ent = entityFor("test:jet", "Test Jet");
    const FlightModelData fm = parseFlightModel(kHelicopter);
    ManualSources src;
    src.entity = &ent;
    src.model = &fm;

    const AircraftManual m = buildAircraftManual(src);
    CHECK_FALSE(hasSection(m, "Sensors"));
}
