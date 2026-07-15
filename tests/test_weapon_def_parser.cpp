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
mesh     = "aim120c"

[seeker]
type            = "active-radar"
sensor_id       = "aim120c-seeker"
fire_and_forget = true
pitbull_nm      = 10
loft_bias_deg   = 20
loft_range_nm   = 15

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

// The pre-#583 seeker form: an ad-hoc lobe on the weapon instead of a sensor-def reference.
// Deprecated (one release), still parsed — existing packs must keep loading.
const std::string kLegacySeekerMissile = R"toml(
[weapon]
id       = "aim9l"
name     = "AIM-9L Sidewinder"
type     = "missile"
category = "air-to-air"

[seeker]
type            = "ir"
fov_deg         = 25
acquisition_nm  = 5
fire_and_forget = true

[performance]
max_range_nm = 9

[warhead]
blast_radius_ft = 30
damage          = 60

[load]
weight_lb   = 190
drag_factor = 0.001
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

    CHECK(w.mesh == "aim120c");

    REQUIRE(w.seeker.has_value());
    CHECK(w.seeker->type == SeekerType::ActiveRadar);
    CHECK(w.seeker->sensorId == "aim120c-seeker");
    CHECK_FALSE(w.seeker->usesLegacyLobe());
    CHECK(w.seeker->fireAndForget);
    CHECK_FALSE(w.seeker->requiresDesignator);
    CHECK_THAT(w.seeker->pitbullRangeM, WithinRel(10.f * 1852.f, 1e-4f));
    CHECK_THAT(w.seeker->loftBiasDeg, WithinAbs(20.f, 1e-4f));
    CHECK_THAT(w.seeker->loftRangeM, WithinRel(15.f * 1852.f, 1e-4f));

    CHECK_THAT(w.performance.maxG, WithinAbs(30.f, 1e-4f));
    CHECK_THAT(w.performance.motorBurnTimeS, WithinAbs(4.5f, 1e-4f));
    CHECK_THAT(w.countermeasures.chaff, WithinAbs(0.4f, 1e-4f));
    CHECK_THAT(w.countermeasures.notch, WithinAbs(0.6f, 1e-4f));
    CHECK_THAT(w.countermeasures.flare, WithinAbs(0.f, 1e-4f)); // absent = 0
}

TEST_CASE("Authored aviation units are converted to SI", "[weapon]") {
    const WeaponDef w = parseWeaponDef(kMissile);

    // 30 nm, 0.5 nm, and the seeker's pitbull/loft nm
    CHECK_THAT(w.performance.maxRangeM, WithinRel(30.f * 1852.f, 1e-4f));
    CHECK_THAT(w.performance.minRangeM, WithinRel(0.5f * 1852.f, 1e-4f));
    CHECK_THAT(w.seeker->pitbullRangeM, WithinRel(10.f * 1852.f, 1e-4f));
    // The legacy lobe's nm conversion still works while the deprecated form parses.
    CHECK_THAT(parseWeaponDef(kLegacySeekerMissile).seeker->acquisitionRangeM, WithinRel(5.f * 1852.f, 1e-4f));
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

TEST_CASE("A fuel (drop-tank) store needs no performance or warhead — mass and drag only (#862)", "[weapon]") {
    const std::string toml = R"toml(
[weapon]
id       = "fl-base:tank370"
name     = "370 gal drop tank"
type     = "fuel"
category = "air-to-air"

[load]
weight_lb   = 2200
drag_factor = 0.008
)toml";

    const WeaponDef w = parseWeaponDef(toml);
    CHECK(w.type == WeaponType::Fuel);
    CHECK_FALSE(w.seeker.has_value());
    CHECK(w.performance.maxRangeM == 0.f); // no reach — it is not a weapon
    CHECK(w.warhead.damage == 0.f);        // no warhead
    CHECK(w.load.massKg > 0.f);            // but it does cost the airframe
    CHECK(w.load.dragFactor > 0.f);
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

TEST_CASE("The deprecated legacy seeker lobe still parses, and says so", "[weapon]") {
    // One release of grace: packs authored against the pre-#583 schema keep loading. The parser
    // cannot warn (it has no logger); usesLegacyLobe() is what the bootstrap and validate-weapon
    // key their warnings on.
    const WeaponDef w = parseWeaponDef(kLegacySeekerMissile);
    REQUIRE(w.seeker.has_value());
    CHECK(w.seeker->usesLegacyLobe());
    CHECK(w.seeker->sensorId.empty());
    CHECK_THAT(w.seeker->fovDeg, WithinAbs(25.f, 1e-4f));
}

TEST_CASE("sensor_id and the legacy lobe are mutually exclusive", "[weapon]") {
    std::string s(kLegacySeekerMissile);
    s.replace(s.find("type            = \"ir\""), std::string("type            = \"ir\"").size(),
              "type            = \"ir\"\nsensor_id       = \"x:seeker\"");
    CHECK_THROWS_AS(parseWeaponDef(s), std::runtime_error);
}

TEST_CASE("pitbull_nm only means something on an active-radar seeker", "[weapon]") {
    std::string s(kLegacySeekerMissile);
    s.replace(s.find("fire_and_forget = true"), std::string("fire_and_forget = true").size(),
              "fire_and_forget = true\npitbull_nm      = 5");
    CHECK_THROWS_AS(parseWeaponDef(s), std::runtime_error);
}

TEST_CASE("loft_bias_deg and loft_range_nm come as a pair", "[weapon]") {
    std::string one(kMissile);
    one.replace(one.find("loft_range_nm   = 15"), std::string("loft_range_nm   = 15").size(), "");
    CHECK_THROWS_AS(parseWeaponDef(one), std::runtime_error);
}

TEST_CASE("A nuclear warhead needs a yield, and a yield needs to mean it", "[weapon]") {
    auto withWarhead = [](const char* extra) {
        std::string s(kBomb);
        s.replace(s.find("damage          = 180"), std::string("damage          = 180").size(),
                  "damage          = 180\n" + std::string(extra));
        return s;
    };
    CHECK_THROWS_AS(parseWeaponDef(withWarhead("nuclear = true")), std::runtime_error);
    CHECK_THROWS_AS(parseWeaponDef(withWarhead("yield_kt = 15")), std::runtime_error);

    const WeaponDef w = parseWeaponDef(withWarhead("nuclear = true\nyield_kt = 15"));
    CHECK(w.warhead.nuclear);
    CHECK_THAT(w.warhead.yieldKt, WithinAbs(15.f, 1e-4f));
}

TEST_CASE("rate_of_fire_rpm parses for guns", "[weapon]") {
    const std::string gun = R"toml(
[weapon]
id       = "m61"
name     = "M61A1"
type     = "gun"
category = "air-to-air"

[performance]
max_range_nm     = 0.6
rate_of_fire_rpm = 6000

[warhead]
blast_radius_ft = 3
damage          = 8

[load]
weight_lb   = 250
drag_factor = 0
)toml";
    const WeaponDef w = parseWeaponDef(gun);
    CHECK_THAT(w.performance.rateOfFireRpm, WithinAbs(6000.f, 1e-3f));
    CHECK(w.mesh.empty());
}

TEST_CASE("[seeker] and [guidance] are mutually exclusive", "[weapon]") {
    const std::string toml = "[weapon]\nid=\"x\"\nname=\"X\"\ntype=\"missile\"\ncategory=\"air-to-air\"\n"
                             "[seeker]\ntype=\"ir\"\n[guidance]\ntype=\"laser\"\n"
                             "[performance]\nmax_range_nm=10\n[warhead]\nblast_radius_ft=10\ndamage=10\n"
                             "[load]\nweight_lb=100\ndrag_factor=0.01\n";
    CHECK_THROWS_AS(parseWeaponDef(toml), std::runtime_error);
}

TEST_CASE("The builtin sandbox weapons are well-formed", "[weapon]") {
    const WeaponDef& gun = BuiltinWeapon::cannon();
    CHECK(gun.id == "builtin:cannon");
    CHECK(gun.type == WeaponType::Gun);
    CHECK_FALSE(gun.seeker.has_value());
    CHECK(gun.performance.rateOfFireRpm > 0.f);
    CHECK(gun.load.rounds > 1u); // a gun carries a magazine, not a single shot

    const WeaponDef& ir = BuiltinWeapon::irMissile();
    CHECK(ir.id == "builtin:ir-missile");
    CHECK(ir.type == WeaponType::Missile);
    REQUIRE(ir.seeker.has_value());
    CHECK(ir.seeker->type == SeekerType::Infrared);
    CHECK(ir.seeker->sensorId == "builtin:ir-seeker"); // the one-vocabulary reference, not a legacy lobe
    CHECK_FALSE(ir.seeker->usesLegacyLobe());
    CHECK(ir.load.rounds == 1u);

    const WeaponDef& rdr = BuiltinWeapon::radarMissile();
    CHECK(rdr.id == "builtin:radar-missile");
    REQUIRE(rdr.seeker.has_value());
    CHECK(rdr.seeker->type == SeekerType::ActiveRadar);
    CHECK(rdr.seeker->sensorId == "builtin:radar-seeker");
    CHECK(rdr.seeker->pitbullRangeM > 0.f); // ARH goes active on its own radar (#628)
    CHECK(rdr.seeker->loftRangeM > 0.f);
    CHECK(rdr.performance.maxRangeM > ir.performance.maxRangeM); // BVR outranges the heater

    // Same object every call (BuiltinFlightModel pattern).
    CHECK(&BuiltinWeapon::cannon() == &gun);
}
