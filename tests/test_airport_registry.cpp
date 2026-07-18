// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "flight/Geodetic.h"
#include "flight/LocalFrame.h"
#include "world/AirportDef.h"
#include "world/AirportDefParser.h"
#include "world/AirportRegistry.h"
#include "world/BuiltinAirport.h"

#include <glm/geometric.hpp>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

using namespace fl;
using Catch::Approx;

namespace {

// A geodetic airport def with one runway.
AirportDef geodeticDef(std::string id, double latDeg, double lonDeg, double elevM, float hdg = 90.f,
                       float len = 3000.f) {
    AirportDef def;
    def.id = std::move(id);
    def.name = "Test Field";
    def.latRad = latDeg * (std::numbers::pi / 180.0);
    def.lonRad = lonDeg * (std::numbers::pi / 180.0);
    def.elevationM = elevM;
    def.runways.push_back(RunwayDef{hdg, len, 60.f, RunwaySurface::Concrete});
    return def;
}

} // namespace

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

TEST_CASE("AirportDefParser: full geodetic def round-trips every field", "[airport]") {
    const std::string toml = R"(
[airport]
id = "fl-base:homeplate"
name = "Homeplate AFB"
lat = 36.23
lon = -115.03
elevation_m = 569.0
accepts_landings = true
faction = "usa"

[[runway]]
heading_deg = 90.0
length_m = 2500
width_m = 45
surface = "asphalt"

[[runway]]
heading_deg = 180.0
length_m = 3000
width_m = 60
surface = "concrete"
)";
    const AirportDef def = parseAirportDef(toml);
    CHECK(def.id == "fl-base:homeplate");
    CHECK(def.name == "Homeplate AFB");
    CHECK_FALSE(def.useWorldXZ);
    CHECK(def.latRad == Approx(36.23 * std::numbers::pi / 180.0));
    CHECK(def.lonRad == Approx(-115.03 * std::numbers::pi / 180.0));
    CHECK(def.elevationM == Approx(569.0));
    CHECK(def.acceptsLandings);
    CHECK(def.factionId == "usa");
    REQUIRE(def.runways.size() == 2u);
    CHECK(def.runways[0].headingDeg == Approx(90.0));
    CHECK(def.runways[0].lengthM == Approx(2500.0));
    CHECK(def.runways[0].surface == RunwaySurface::Asphalt);
    CHECK(def.runways[1].surface == RunwaySurface::Concrete);
}

TEST_CASE("AirportDefParser: defaults", "[airport]") {
    const std::string toml = R"(
[airport]
id = "x:strip"
name = "Strip"
world_x = 1000.0
world_z = 2000.0
[[runway]]
heading_deg = 0.0
length_m = 800
width_m = 20
)";
    const AirportDef def = parseAirportDef(toml);
    CHECK(def.useWorldXZ);
    CHECK(def.worldX == Approx(1000.0));
    CHECK(def.worldZ == Approx(2000.0));
    CHECK(def.elevationM < 0.0); // absent = resolve from terrain
    CHECK(def.acceptsLandings);  // default true
    CHECK(def.factionId.empty());
    CHECK(def.runways[0].surface == RunwaySurface::Asphalt); // default
}

TEST_CASE("AirportDefParser: rejects malformed defs", "[airport]") {
    CHECK_THROWS(parseAirportDef("[airport]\nname=\"x\"\nlat=1\nlon=2\n")); // missing id
    CHECK_THROWS(parseAirportDef("[airport]\nid=\"x\"\nlat=1\nlon=2\n"));   // missing name
    CHECK_THROWS(parseAirportDef("[airport]\nid=\"x\"\nname=\"y\"\n"));     // no placement
    CHECK_THROWS(
        parseAirportDef("[airport]\nid=\"x\"\nname=\"y\"\nlat=1\nlon=2\nworld_x=1\nworld_z=2\n")); // both placements
    CHECK_THROWS(parseAirportDef("[airport]\nid=\"x\"\nname=\"y\"\nlat=1\nlon=2\n"
                                 "[[runway]]\nheading_deg=0\nlength_m=-5\nwidth_m=20\n")); // negative length
    CHECK_THROWS(
        parseAirportDef("[airport]\nid=\"x\"\nname=\"y\"\nlat=1\nlon=2\n"
                        "[[runway]]\nheading_deg=0\nlength_m=5\nwidth_m=20\nsurface=\"lava\"\n")); // bad surface
}

TEST_CASE("runwaySurfaceFromString round-trips runwaySurfaceName", "[airport]") {
    for (auto s : {RunwaySurface::Concrete, RunwaySurface::Asphalt, RunwaySurface::Grass, RunwaySurface::Gravel,
                   RunwaySurface::Water, RunwaySurface::Deck}) {
        RunwaySurface parsed{};
        REQUIRE(runwaySurfaceFromString(runwaySurfaceName(s), parsed));
        CHECK(parsed == s);
    }
    RunwaySurface dummy{};
    CHECK_FALSE(runwaySurfaceFromString("dirt-road", dummy));
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

TEST_CASE("AirportRegistry: empty registry is safe", "[airport]") {
    AirportRegistry reg;
    CHECK(reg.count() == 0u);
    CHECK(reg.byId("nope") == nullptr);
    CHECK(reg.nearestTo(0, 0, 1e9) == nullptr);
}

TEST_CASE("AirportRegistry: byId, count, forEach", "[airport]") {
    std::vector<AirportDef> defs{geodeticDef("a", 10, 20, 100), geodeticDef("b", -30, 40, 200)};
    AirportRegistry reg;
    reg.load(std::move(defs), kEarthRadiusM, nullptr);
    CHECK(reg.count() == 2u);
    REQUIRE(reg.byId("a") != nullptr);
    CHECK(reg.byId("a")->elevationM == Approx(100.0));
    CHECK(reg.byId("b")->elevationM == Approx(200.0));
    CHECK(reg.byId("missing") == nullptr);

    int seen = 0;
    reg.forEach([&](const ResolvedAirport&) { ++seen; });
    CHECK(seen == 2);
}

TEST_CASE("AirportRegistry: first id wins on duplicate", "[airport]") {
    std::vector<AirportDef> defs{geodeticDef("dup", 10, 20, 111), geodeticDef("dup", 10, 20, 999)};
    AirportRegistry reg;
    reg.load(std::move(defs), kEarthRadiusM, nullptr);
    CHECK(reg.count() == 1u);
    CHECK(reg.byId("dup")->elevationM == Approx(111.0)); // the first def
}

TEST_CASE("AirportRegistry: geodetic placement matches geodeticToWorld", "[airport]") {
    AirportRegistry reg;
    std::vector<AirportDef> defs{geodeticDef("kfoo", 34.05, -118.24, 40.0)};
    reg.load(std::move(defs), kEarthRadiusM, nullptr);
    const ResolvedAirport* a = reg.byId("kfoo");
    REQUIRE(a != nullptr);
    double x = 0, y = 0, z = 0;
    geodeticToWorld(LatLonAlt{34.05 * std::numbers::pi / 180.0, -118.24 * std::numbers::pi / 180.0, 40.0}, x, y, z,
                    kEarthRadiusM);
    CHECK(a->worldPos.x == Approx(x));
    CHECK(a->worldPos.y == Approx(y));
    CHECK(a->worldPos.z == Approx(z));
    // The resolved world position sits at the field elevation above the datum.
    CHECK(geodeticAltitude(a->worldPos.x, a->worldPos.y, a->worldPos.z, kEarthRadiusM) == Approx(40.0));
}

TEST_CASE("AirportRegistry: world-XZ placement lands near the origin", "[airport]") {
    AirportDef def;
    def.id = "sandbox";
    def.name = "Sandbox";
    def.useWorldXZ = true;
    def.worldX = 4000.0;
    def.worldZ = 0.0;
    def.elevationM = 500.0;
    def.runways.push_back(RunwayDef{90.f, 2500.f, 45.f, RunwaySurface::Asphalt});
    AirportRegistry reg;
    reg.load({def}, kEarthRadiusM, nullptr);
    const ResolvedAirport* a = reg.byId("sandbox");
    REQUIRE(a != nullptr);
    CHECK(a->worldPos.x == Approx(4000.0));
    CHECK(a->worldPos.z == Approx(0.0));
    // Near-side surface at elevation 500 m: altitude above the datum ~= 500 m.
    CHECK(geodeticAltitude(a->worldPos.x, a->worldPos.y, a->worldPos.z, kEarthRadiusM) == Approx(500.0).margin(0.01));
}

TEST_CASE("AirportRegistry: elevation resolves from injected height fn only when unset", "[airport]") {
    bool called = false;
    AirportRegistry::HeightFn h = [&](glm::dvec3) {
        called = true;
        return 123.0;
    };
    // elevationM = -1 -> resolve via heightFn.
    {
        AirportRegistry reg;
        reg.load({geodeticDef("resolve", 0, 0, -1.0)}, kEarthRadiusM, h);
        CHECK(called);
        CHECK(reg.byId("resolve")->elevationM == Approx(123.0));
    }
    // Explicit elevation ignores heightFn.
    {
        called = false;
        AirportRegistry reg;
        reg.load({geodeticDef("explicit", 0, 0, 77.0)}, kEarthRadiusM, h);
        CHECK_FALSE(called);
        CHECK(reg.byId("explicit")->elevationM == Approx(77.0));
    }
}

TEST_CASE("AirportRegistry: runway geometry is length-correct and heading-correct", "[airport]") {
    AirportRegistry reg;
    reg.load({geodeticDef("rw", 0, 0, 0, /*hdg=*/45.f, /*len=*/3000.f)}, kEarthRadiusM, nullptr);
    const ResolvedRunway& r = reg.byId("rw")->runways.at(0);
    // Threshold->far-end distance equals the runway length.
    CHECK(glm::length(r.oppositeEnd - r.threshold) == Approx(3000.0).margin(0.5));
    // Both ends at (approximately) the field elevation.
    CHECK(geodeticAltitude(r.threshold.x, r.threshold.y, r.threshold.z, kEarthRadiusM) == Approx(0.0).margin(1.0));
    // The centerline points along the requested heading (45 deg) in the local tangent frame.
    const float hdg = headingTo(r.threshold, r.oppositeEnd, kEarthRadiusM);
    CHECK(hdg == Approx(45.0 * std::numbers::pi / 180.0).margin(0.02));
}

TEST_CASE("AirportRegistry: nearestTo respects range", "[airport]") {
    AirportRegistry reg;
    std::vector<AirportDef> defs;
    // Two world-XZ fields at known planar offsets near the origin.
    for (auto [id, x] : std::initializer_list<std::pair<const char*, double>>{{"near", 1000.0}, {"far", 50000.0}}) {
        AirportDef d;
        d.id = id;
        d.name = id;
        d.useWorldXZ = true;
        d.worldX = x;
        d.worldZ = 0.0;
        d.elevationM = 0.0;
        defs.push_back(d);
    }
    reg.load(std::move(defs), kEarthRadiusM, nullptr);
    // From near the origin, "near" is the closest.
    const ResolvedAirport* got = reg.nearestTo(0.0, 0.0, 1e6);
    REQUIRE(got != nullptr);
    CHECK(got->def.id == "near");
    // With a 2 km cap, only "near" qualifies.
    CHECK(reg.nearestTo(0.0, 0.0, 2000.0) != nullptr);
    // With a 500 m cap, nothing is in range.
    CHECK(reg.nearestTo(0.0, 0.0, 500.0) == nullptr);
}

TEST_CASE("builtinAirfield is a valid sandbox strip", "[airport]") {
    const AirportDef def = builtinAirfield();
    CHECK(def.id == "builtin:airfield");
    CHECK(def.useWorldXZ);
    CHECK(def.acceptsLandings);
    CHECK(def.elevationM < 0.0); // terrain-resolved
    REQUIRE(def.runways.size() == 1u);
    CHECK(def.runways[0].lengthM == Approx(2500.0));
    CHECK(def.runways[0].widthM == Approx(45.0));
    CHECK(def.runways[0].headingDeg == Approx(90.0));
    CHECK(def.runways[0].surface == RunwaySurface::Asphalt);
}
