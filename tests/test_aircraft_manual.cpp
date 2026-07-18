// SPDX-License-Identifier: GPL-3.0-or-later
//
// The generated aircraft manual (#821).
//
// THE DESIGN CLAIM UNDER TEST: the manual is a consumer of the same sources of truth as the
// simulation, so it cannot disagree with the aircraft you are flying. The alternative -- a
// hand-written page per aircraft -- duplicates every number the flight model already contains and
// drifts from it silently. These tests are what hold that line.

#include "manual/AircraftManual.h"

#include "entity/EntityDef.h"
#include "flight/FlightModelParser.h"
#include "sensor/SensorDef.h"
#include "sensor/SensorDefParser.h"
#include "weapon/WeaponDef.h"
#include "weapon/WeaponRegistry.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace fl;

namespace {

const char* kJet = R"(
[aircraft]
name         = "Test Light Fighter"
type         = "fighter"
engine_type  = "turbojet"
has_fbw      = false
cruise_alt_m = 11000.0

[flight_model]
mass_kg      = 4349.0
wing_area_m2 = 17.28
wingspan_m   = 8.13
mac_m        = 2.44
fuel_kg      = 2000.0
ixx_kg_m2    = 3800.0
iyy_kg_m2    = 25000.0
izz_kg_m2    = 27000.0

[aero.cl_table]
alpha  = [-5.0, 0.0, 5.0, 10.0, 15.0, 18.0, 22.0, 26.0]
mach   = [0.3, 0.6, 0.9, 1.2]
values = [
    -0.25,-0.26,-0.28,-0.20,
     0.05, 0.05, 0.06, 0.04,
     0.40, 0.43, 0.48, 0.35,
     0.78, 0.84, 0.92, 0.66,
     1.10, 1.18, 1.28, 0.92,
     1.24, 1.32, 1.42, 1.02,
     1.15, 1.22, 1.30, 0.94,
     0.95, 1.00, 1.08, 0.78,
]

[aero.drag_polar]
cd0           = 0.0200
k             = 0.115
speedbrake_cd = 0.06
gear_cd       = 0.025

[aero.moments]
cm_alpha = -0.6
cm_q     = -9.0
cm_de    = -0.9
cl_beta  = -0.09
cl_p     = -0.38
cl_da    =  0.08
cn_beta  =  0.12
cn_r     = -0.14
cn_dr    = -0.06

[aero.limits]
alpha_stall_deg  = 18.0
max_g_structural =  7.33
min_g_structural = -3.0
max_mach         =  1.63

[aero.controls]
max_elevator_deg = 25.0
max_aileron_deg  = 20.0
max_rudder_deg   = 30.0

[engine]
fuel_flow_idle_kg_s = 0.05
fuel_flow_mil_kg_s  = 0.52
fuel_flow_ab_kg_s   = 1.55
spool_time_s        = 4.0

[engine.mil_thrust]
mach   = [0.0, 0.9]
alt_km = [0.0, 11.0]
values = [22.2, 6.4,
          20.4, 7.6]

[engine.ab_thrust]
mach   = [0.0, 0.9]
alt_km = [0.0, 11.0]
values = [31.2, 9.6,
          36.0, 12.6]
)";

EntityDef makeEntity() {
    EntityDef def;
    def.id = "fl-base:f5e";
    def.name = "F-5E Tiger II";
    def.sensorIds = {"fl-base:apq159"};

    Hardpoint tip;
    tip.slot = 1;
    tip.allowed = {"fl-base:aim9p"};
    tip.defaultWeapon = "fl-base:aim9p";

    Hardpoint gun;
    gun.slot = 2;
    gun.allowed = {"fl-base:m39a2"};
    gun.defaultWeapon = "fl-base:m39a2";

    def.hardpoints = {tip, gun};
    return def;
}

WeaponRegistry makeWeapons() {
    WeaponRegistry reg;
    WeaponDef aim9;
    aim9.id = "fl-base:aim9p";
    aim9.name = "AIM-9P Sidewinder";
    aim9.type = WeaponType::Missile;
    aim9.load.massKg = 85.5f;
    aim9.load.dragFactor = 0.0012f;
    reg.registerWeapon(aim9);

    WeaponDef gun;
    gun.id = "fl-base:m39a2";
    gun.name = "M39A2 20 mm cannon";
    gun.type = WeaponType::Gun;
    gun.load.massKg = 102.f;
    reg.registerWeapon(gun);
    return reg;
}

sensor::SensorDef makeRadar() {
    return sensor::parseSensorDef("[sensor]\n"
                                  "id = \"fl-base:apq159\"\n"
                                  "name = \"AN/APQ-159\"\n"
                                  "type = \"radar\"\n"
                                  "emitter = true\n"
                                  "\n[search]\n"
                                  "az_half_angle_deg = 60.0\n"
                                  "el_half_angle_deg = 30.0\n"
                                  "max_range_nm = 20.0\n"
                                  "pod = 0.75\n");
}

const ManualSection* findSection(const AircraftManual& m, std::string_view titlePrefix) {
    for (const auto& s : m.sections)
        if (s.title.rfind(titlePrefix, 0) == 0)
            return &s;
    return nullptr;
}

bool anyValueContains(const ManualSection& s, std::string_view needle) {
    for (const auto& r : s.rows)
        if (r.value.find(needle) != std::string::npos)
            return true;
    return false;
}

std::string valueOf(const ManualSection& s, std::string_view labelNeedle) {
    for (const auto& r : s.rows)
        if (r.label.find(labelNeedle) != std::string::npos)
            return r.value;
    return {};
}

} // namespace

TEST_CASE("manual: performance is COMPUTED from the flight model, not transcribed", "[manual]") {
    const FlightModelData fm = parseFlightModel(kJet);
    const EntityDef def = makeEntity();

    ManualSources src;
    src.entity = &def;
    src.model = &fm;

    const AircraftManual m = buildAircraftManual(src);

    CHECK(m.title == "F-5E Tiger II");

    const ManualSection* perf = findSection(m, "Performance");
    REQUIRE(perf != nullptr);
    CHECK_FALSE(perf->rows.empty());
    CHECK_FALSE(valueOf(*perf, "stall speed").empty());
    CHECK_FALSE(valueOf(*perf, "corner speed").empty());
    CHECK_FALSE(valueOf(*perf, "sustained turn").empty());
}

TEST_CASE("manual: RETUNING THE DRAG POLAR CHANGES THE MANUAL, with no other edit", "[manual]") {
    // This is #821's headline acceptance criterion, and the reason the manual is generated rather
    // than written. A hand-authored page would still be claiming the old corner speed here.
    const EntityDef def = makeEntity();

    const FlightModelData sleek = parseFlightModel(kJet);

    std::string draggyToml = kJet;
    auto pos = draggyToml.find("cd0           = 0.0200");
    REQUIRE(pos != std::string::npos);
    draggyToml.replace(pos, std::string("cd0           = 0.0200").size(), "cd0           = 0.0600");
    const FlightModelData draggy = parseFlightModel(draggyToml);

    ManualSources a;
    a.entity = &def;
    a.model = &sleek;
    ManualSources b;
    b.entity = &def;
    b.model = &draggy;

    const AircraftManual mSleek = buildAircraftManual(a);
    const AircraftManual mDraggy = buildAircraftManual(b);

    const ManualSection* pSleek = findSection(mSleek, "Performance");
    const ManualSection* pDraggy = findSection(mDraggy, "Performance");
    REQUIRE(pSleek != nullptr);
    REQUIRE(pDraggy != nullptr);

    // Tripling parasite drag must move the numbers. If this ever passes with them equal, the manual
    // has stopped reading the flight model and started making things up.
    CHECK(valueOf(*pSleek, "max level speed") != valueOf(*pDraggy, "max level speed"));
    CHECK(valueOf(*pSleek, "sustained turn") != valueOf(*pDraggy, "sustained turn"));
}

TEST_CASE("manual: limits come straight from [aero.limits]", "[manual]") {
    const FlightModelData fm = parseFlightModel(kJet);
    const EntityDef def = makeEntity();

    ManualSources src;
    src.entity = &def;
    src.model = &fm;
    const AircraftManual m = buildAircraftManual(src);

    const ManualSection* limits = findSection(m, "Limits");
    REQUIRE(limits != nullptr);
    CHECK(valueOf(*limits, "structural limit").find("7.33") != std::string::npos);
    CHECK(valueOf(*limits, "never exceed").find("1.63") != std::string::npos);

    // A pilot flying a 1972 airframe needs to be told, in as many words, that nothing will stop them
    // from breaking it (#816).
    CHECK(valueOf(*limits, "G-limiter").find("NONE") != std::string::npos);
}

TEST_CASE("manual: an FBW aircraft says its limiter will hold it", "[manual]") {
    FlightModelData fm = parseFlightModel(kJet);
    fm.meta.has_fbw = true;
    const EntityDef def = makeEntity();

    ManualSources src;
    src.entity = &def;
    src.model = &fm;
    const AircraftManual m = buildAircraftManual(src);

    const ManualSection* limits = findSection(m, "Limits");
    REQUIRE(limits != nullptr);
    CHECK(valueOf(*limits, "G-limiter").find("fly-by-wire") != std::string::npos);
}

TEST_CASE("manual: stations name their stores from the weapon registry", "[manual]") {
    const FlightModelData fm = parseFlightModel(kJet);
    const EntityDef def = makeEntity();
    const WeaponRegistry weapons = makeWeapons();

    ManualSources src;
    src.entity = &def;
    src.model = &fm;
    src.weapons = &weapons;
    src.payload = PayloadEffect{187.5f, 0.0012f};

    const AircraftManual m = buildAircraftManual(src);

    const ManualSection* stations = findSection(m, "Stations");
    REQUIRE(stations != nullptr);
    CHECK(anyValueContains(*stations, "AIM-9P Sidewinder"));
    CHECK(anyValueContains(*stations, "M39A2"));
    CHECK(anyValueContains(*stations, "86 kg")); // 85.5 kg, rounded for display
}

TEST_CASE("manual: an unresolvable store is reported, not hidden", "[manual]") {
    const FlightModelData fm = parseFlightModel(kJet);
    const EntityDef def = makeEntity();
    WeaponRegistry empty; // nobody has heard of these weapons

    ManualSources src;
    src.entity = &def;
    src.model = &fm;
    src.weapons = &empty;

    const AircraftManual m = buildAircraftManual(src);
    const ManualSection* stations = findSection(m, "Stations");
    REQUIRE(stations != nullptr);
    CHECK(anyValueContains(*stations, "unknown store"));
}

TEST_CASE("manual: a crewed aircraft gets a generated Crew section (#977)", "[manual]") {
    const FlightModelData fm = parseFlightModel(kJet);

    // A two-seat bomber: a Fly+Fire pilot, and a Fire tail-gunner aiming a rear turret.
    EntityDef def;
    def.id = "test:bomber";
    def.name = "Bomber";
    Hardpoint bombBay;
    bombBay.slot = 0;
    bombBay.allowed = {"x"};
    bombBay.defaultWeapon = "";
    Hardpoint tailGun;
    tailGun.slot = 1;
    tailGun.allowed = {"x"};
    tailGun.defaultWeapon = "";
    def.hardpoints = {bombBay, tailGun};

    TurretDef tail;
    tail.id = "tail";
    tail.azMinDeg = -80.f;
    tail.azMaxDeg = 80.f;
    tail.elMinDeg = -10.f;
    tail.elMaxDeg = 80.f;
    tail.stations = {1};
    def.turrets = {tail};

    SeatDef pilot;
    pilot.role = "pilot";
    pilot.capabilities =
        withCapability(withCapability(CrewCapabilityMask{0}, CrewCapability::Fly), CrewCapability::Fire);
    pilot.stations = {0};
    SeatDef gunner;
    gunner.role = "tail-gunner";
    gunner.capabilities = withCapability(CrewCapabilityMask{0}, CrewCapability::Fire);
    gunner.turret = "tail";
    def.crew = {pilot, gunner};

    ManualSources src;
    src.entity = &def;
    src.model = &fm;

    const AircraftManual m = buildAircraftManual(src);
    const ManualSection* crew = findSection(m, "Crew");
    REQUIRE(crew != nullptr);
    REQUIRE(crew->rows.size() == 2u);
    CHECK(valueOf(*crew, "pilot").find("fly") != std::string::npos);
    CHECK(valueOf(*crew, "pilot").find("fire") != std::string::npos);
    // The gunner row names its turret arc (generated from the TurretDef).
    const std::string gunnerRow = valueOf(*crew, "tail-gunner");
    CHECK(gunnerRow.find("fire") != std::string::npos);
    CHECK(gunnerRow.find("turret \"tail\"") != std::string::npos);
    CHECK(gunnerRow.find("az") != std::string::npos);
}

TEST_CASE("manual: a single-seat aircraft has no Crew section (#977)", "[manual]") {
    const FlightModelData fm = parseFlightModel(kJet);
    const EntityDef def = makeEntity(); // no [[crew]] -> implicit single pilot
    ManualSources src;
    src.entity = &def;
    src.model = &fm;
    const AircraftManual m = buildAircraftManual(src);
    CHECK(findSection(m, "Crew") == nullptr);
}

TEST_CASE("manual: the sensor section is built from resolved sensor defs", "[manual]") {
    const FlightModelData fm = parseFlightModel(kJet);
    const EntityDef def = makeEntity();
    const sensor::SensorDef radar = makeRadar();

    ManualSources src;
    src.entity = &def;
    src.model = &fm;
    src.sensors = {&radar};

    const AircraftManual m = buildAircraftManual(src);
    const ManualSection* sensors = findSection(m, "Sensors");
    REQUIRE(sensors != nullptr);
    REQUIRE(sensors->rows.size() == 1);
    CHECK(sensors->rows[0].label == "AN/APQ-159");
    CHECK(sensors->rows[0].value.find("radar") != std::string::npos);
    CHECK(sensors->rows[0].value.find("20 nm") != std::string::npos);
    // An emitting radar tells the enemy where you are, and the manual should say so (#679).
    CHECK(sensors->rows[0].value.find("emits") != std::string::npos);
}

TEST_CASE("manual: a pack's prose is carried verbatim, and is optional", "[manual]") {
    const FlightModelData fm = parseFlightModel(kJet);
    const EntityDef def = makeEntity();

    ManualSources withProse;
    withProse.entity = &def;
    withProse.model = &fm;
    withProse.prose = "# Employment\nIt bleeds energy in a sustained fight.\nKeep it fast.\n";

    const AircraftManual m = buildAircraftManual(withProse);
    REQUIRE(m.prose.size() == 3);
    CHECK(m.prose[1] == "It bleeds energy in a sustained fight.");

    // A pack that ships no prose still gets a complete manual -- every number in it is generated.
    ManualSources noProse;
    noProse.entity = &def;
    noProse.model = &fm;
    const AircraftManual bare = buildAircraftManual(noProse);
    CHECK(bare.prose.empty());
    CHECK_FALSE(bare.sections.empty());
    CHECK(findSection(bare, "Performance") != nullptr);
}

TEST_CASE("manual: a community pack's aircraft gets a manual with no engine change", "[manual]") {
    // The whole point of generating it: an aircraft nobody has ever seen, from a pack nobody wrote
    // code for, produces a complete and correct reference.
    FlightModelData fm = parseFlightModel(kJet);
    fm.meta.name = "Some Modder's Jet";

    EntityDef def;
    def.id = "community:thing";
    def.name = "Some Modder's Jet";

    ManualSources src;
    src.entity = &def;
    src.model = &fm;

    const AircraftManual m = buildAircraftManual(src);
    CHECK(m.title == "Some Modder's Jet");
    CHECK(findSection(m, "Airframe") != nullptr);
    CHECK(findSection(m, "Limits") != nullptr);
    CHECK(findSection(m, "Performance") != nullptr);
}
