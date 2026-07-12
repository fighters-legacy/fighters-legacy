// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "weapon/BuiltinWeapon.h"
#include "weapon/WeaponDefParser.h"

#include <stdexcept>
#include <string>

using namespace fl;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// The AIM-120C example from docs/modding/formats.md, verbatim.
const std::string kMissile = R"toml(
[weapon]
id       = "aim120c"
name     = "AIM-120C AMRAAM"
type     = "missile"
category = "air-to-air"

[seeker]
type            = "active-radar"
fov_deg         = 60
acquisition_nm  = 20
fire_and_forget = true

[performance]
max_range_nm      = 30
min_range_nm      = 0.5
max_speed_kts     = 2400
motor_burn_time_s = 4.5
max_g             = 30

[warhead]
blast_radius_ft = 50
damage          = 100

[countermeasures]
chaff_susceptibility = 0.4
notch_susceptibility = 0.6

[load]
weight_lb   = 335
drag_factor = 0.008
)toml";

// The GBU-12 example from docs/modding/formats.md, verbatim.
const std::string kBomb = R"toml(
[weapon]
id       = "gbu12"
name     = "GBU-12 Paveway II"
type     = "bomb"
category = "air-to-ground"

[guidance]
type                = "laser"
requires_designator = true

[performance]
standoff_range_ft = 15000
CEP_ft            = 8

[warhead]
blast_radius_ft = 100
damage          = 180

[load]
weight_lb   = 500
drag_factor = 0.020
)toml";

} // namespace

TEST_CASE("Parses the documented missile example", "[weapon]") {
    const WeaponDef w = parseWeaponDef(kMissile);

    CHECK(w.id == "aim120c");
    CHECK(w.name == "AIM-120C AMRAAM");
    CHECK(w.type == WeaponType::Missile);
    CHECK(w.category == WeaponCategory::AirToAir);

    REQUIRE(w.seeker.has_value());
    CHECK(w.seeker->type == SeekerType::ActiveRadar);
    CHECK_THAT(w.seeker->fovDeg, WithinAbs(60.f, 1e-4f));
    CHECK(w.seeker->fireAndForget);
    CHECK_FALSE(w.seeker->requiresDesignator);

    CHECK_THAT(w.performance.maxG, WithinAbs(30.f, 1e-4f));
    CHECK_THAT(w.performance.motorBurnTimeS, WithinAbs(4.5f, 1e-4f));
    CHECK_THAT(w.countermeasures.chaff, WithinAbs(0.4f, 1e-4f));
    CHECK_THAT(w.countermeasures.notch, WithinAbs(0.6f, 1e-4f));
    CHECK_THAT(w.countermeasures.flare, WithinAbs(0.f, 1e-4f)); // absent = 0
}

TEST_CASE("Authored aviation units are converted to SI", "[weapon]") {
    const WeaponDef w = parseWeaponDef(kMissile);

    // 30 nm, 0.5 nm, 20 nm
    CHECK_THAT(w.performance.maxRangeM, WithinRel(30.f * 1852.f, 1e-4f));
    CHECK_THAT(w.performance.minRangeM, WithinRel(0.5f * 1852.f, 1e-4f));
    CHECK_THAT(w.seeker->acquisitionRangeM, WithinRel(20.f * 1852.f, 1e-4f));
    // 2400 kts
    CHECK_THAT(w.performance.maxSpeedMps, WithinRel(2400.f * 0.514444f, 1e-4f));
    // 50 ft
    CHECK_THAT(w.warhead.blastRadiusM, WithinRel(50.f * 0.3048f, 1e-4f));
    // 335 lb
    CHECK_THAT(w.load.massKg, WithinRel(335.f * 0.45359237f, 1e-4f));
    CHECK_THAT(w.load.dragFactor, WithinAbs(0.008f, 1e-6f));
}

TEST_CASE("Parses the documented bomb example, whose shape differs from a missile", "[weapon]") {
    const WeaponDef w = parseWeaponDef(kBomb);

    CHECK(w.type == WeaponType::Bomb);
    CHECK(w.category == WeaponCategory::AirToGround);

    // [guidance] parses into the same struct as [seeker].
    REQUIRE(w.seeker.has_value());
    CHECK(w.seeker->type == SeekerType::Laser);
    CHECK(w.seeker->requiresDesignator);
    CHECK_FALSE(w.seeker->fireAndForget);

    // standoff_range_ft is the bomb's range; CEP is its accuracy.
    CHECK_THAT(w.performance.maxRangeM, WithinRel(15000.f * 0.3048f, 1e-4f));
    CHECK_THAT(w.performance.cepM, WithinRel(8.f * 0.3048f, 1e-4f));
    // Unpowered: no motor, no manoeuvre limit.
    CHECK_THAT(w.performance.motorBurnTimeS, WithinAbs(0.f, 1e-6f));
    CHECK_THAT(w.performance.maxG, WithinAbs(0.f, 1e-6f));
}

TEST_CASE("An unguided weapon needs no seeker block", "[weapon]") {
    const std::string toml = R"toml(
[weapon]
id       = "mk82"
name     = "Mk-82"
type     = "bomb"
category = "air-to-ground"

[performance]
standoff_range_ft = 2000

[warhead]
blast_radius_ft = 120
damage          = 200

[load]
weight_lb   = 500
drag_factor = 0.022
)toml";

    const WeaponDef w = parseWeaponDef(toml);
    CHECK_FALSE(w.seeker.has_value());
}

TEST_CASE("Missing required blocks and fields throw", "[weapon]") {
    CHECK_THROWS_AS(parseWeaponDef(""), std::runtime_error);
    CHECK_THROWS_AS(parseWeaponDef("this is not toml ["), std::runtime_error);

    SECTION("no [weapon] table") {
        const std::string toml = R"toml(
[performance]
max_range_nm = 10
[warhead]
blast_radius_ft = 10
damage = 10
[load]
weight_lb = 100
drag_factor = 0.01
)toml";
        CHECK_THROWS_AS(parseWeaponDef(toml), std::runtime_error);
    }

    SECTION("no [warhead] table") {
        const std::string toml = R"toml(
[weapon]
id = "x"
name = "X"
type = "missile"
category = "air-to-air"
[performance]
max_range_nm = 10
[load]
weight_lb = 100
drag_factor = 0.01
)toml";
        CHECK_THROWS_AS(parseWeaponDef(toml), std::runtime_error);
    }

    SECTION("no [load] table") {
        const std::string toml = R"toml(
[weapon]
id = "x"
name = "X"
type = "missile"
category = "air-to-air"
[performance]
max_range_nm = 10
[warhead]
blast_radius_ft = 10
damage = 10
)toml";
        CHECK_THROWS_AS(parseWeaponDef(toml), std::runtime_error);
    }
}

TEST_CASE("Unknown enum values throw rather than silently defaulting", "[weapon]") {
    auto withWeaponBlock = [](const char* type, const char* category) {
        return std::string("[weapon]\nid=\"x\"\nname=\"X\"\ntype=\"") + type + "\"\ncategory=\"" + category +
               "\"\n[performance]\nmax_range_nm=10\n[warhead]\nblast_radius_ft=10\ndamage=10\n"
               "[load]\nweight_lb=100\ndrag_factor=0.01\n";
    };

    CHECK_NOTHROW(parseWeaponDef(withWeaponBlock("missile", "air-to-air")));
    CHECK_THROWS_AS(parseWeaponDef(withWeaponBlock("deathray", "air-to-air")), std::runtime_error);
    CHECK_THROWS_AS(parseWeaponDef(withWeaponBlock("missile", "air-to-orbit")), std::runtime_error);
}

TEST_CASE("Out-of-range values throw", "[weapon]") {
    auto perf = [](const char* extra) {
        return std::string("[weapon]\nid=\"x\"\nname=\"X\"\ntype=\"missile\"\n"
                           "category=\"air-to-air\"\n[warhead]\nblast_radius_ft=10\ndamage=10\n"
                           "[load]\nweight_lb=100\ndrag_factor=0.01\n[performance]\n") +
               extra;
    };

    SECTION("negative range") {
        CHECK_THROWS_AS(parseWeaponDef(perf("max_range_nm=-5\n")), std::runtime_error);
    }
    SECTION("min range exceeds max range") {
        CHECK_THROWS_AS(parseWeaponDef(perf("max_range_nm=10\nmin_range_nm=20\n")), std::runtime_error);
    }
    SECTION("zero weight") {
        const std::string toml = "[weapon]\nid=\"x\"\nname=\"X\"\ntype=\"missile\"\ncategory=\"air-to-air\"\n"
                                 "[performance]\nmax_range_nm=10\n[warhead]\nblast_radius_ft=10\ndamage=10\n"
                                 "[load]\nweight_lb=0\ndrag_factor=0.01\n";
        CHECK_THROWS_AS(parseWeaponDef(toml), std::runtime_error);
    }
    SECTION("susceptibility outside [0, 1]") {
        const std::string toml = "[weapon]\nid=\"x\"\nname=\"X\"\ntype=\"missile\"\ncategory=\"air-to-air\"\n"
                                 "[performance]\nmax_range_nm=10\n[warhead]\nblast_radius_ft=10\ndamage=10\n"
                                 "[load]\nweight_lb=100\ndrag_factor=0.01\n"
                                 "[countermeasures]\nchaff_susceptibility=1.5\n";
        CHECK_THROWS_AS(parseWeaponDef(toml), std::runtime_error);
    }
    SECTION("seeker fov outside [0, 180]") {
        const std::string toml = "[weapon]\nid=\"x\"\nname=\"X\"\ntype=\"missile\"\ncategory=\"air-to-air\"\n"
                                 "[seeker]\ntype=\"ir\"\nfov_deg=200\n"
                                 "[performance]\nmax_range_nm=10\n[warhead]\nblast_radius_ft=10\ndamage=10\n"
                                 "[load]\nweight_lb=100\ndrag_factor=0.01\n";
        CHECK_THROWS_AS(parseWeaponDef(toml), std::runtime_error);
    }
}

TEST_CASE("A weapon states its range exactly once", "[weapon]") {
    auto rangeBlock = [](const char* perfBody) {
        return std::string("[weapon]\nid=\"x\"\nname=\"X\"\ntype=\"bomb\"\n"
                           "category=\"air-to-ground\"\n[warhead]\nblast_radius_ft=10\ndamage=10\n"
                           "[load]\nweight_lb=100\ndrag_factor=0.01\n[performance]\n") +
               perfBody;
    };

    // Neither: a weapon with no reach is meaningless.
    CHECK_THROWS_AS(parseWeaponDef(rangeBlock("max_g=5\n")), std::runtime_error);
    // Both: ambiguous — which one is the range?
    CHECK_THROWS_AS(parseWeaponDef(rangeBlock("max_range_nm=10\nstandoff_range_ft=1000\n")), std::runtime_error);
}

TEST_CASE("[seeker] and [guidance] are mutually exclusive", "[weapon]") {
    const std::string toml = "[weapon]\nid=\"x\"\nname=\"X\"\ntype=\"missile\"\ncategory=\"air-to-air\"\n"
                             "[seeker]\ntype=\"ir\"\n[guidance]\ntype=\"laser\"\n"
                             "[performance]\nmax_range_nm=10\n[warhead]\nblast_radius_ft=10\ndamage=10\n"
                             "[load]\nweight_lb=100\ndrag_factor=0.01\n";
    CHECK_THROWS_AS(parseWeaponDef(toml), std::runtime_error);
}

TEST_CASE("The builtin weapon is well-formed for the zero-pack sandbox", "[weapon]") {
    const WeaponDef& w = BuiltinWeapon::get();
    CHECK(w.id == "builtin:test-missile");
    CHECK(w.type == WeaponType::Missile);
    REQUIRE(w.seeker.has_value());
    CHECK(w.seeker->type == SeekerType::Infrared);
    CHECK(w.performance.maxRangeM > 0.f);
    CHECK(w.load.massKg > 0.f);
    // Same object every call (BuiltinFlightModel pattern).
    CHECK(&BuiltinWeapon::get() == &w);
}
