// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "flight/Geodetic.h"
#include "flight/LocalFrame.h"
#include "world/AirportCsvImport.h"
#include "world/AirportDef.h"
#include "world/AirportDefParser.h"
#include "world/AirportIndexFile.h"
#include "world/AirportRegistry.h"
#include "world/BuiltinAirport.h"

#include <cmath>
#include <cstdint>
#include <glm/geometric.hpp>
#include <numbers>
#include <span>
#include <string>
#include <tuple>
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
    CHECK(def.elevationM == Approx(kBuiltinAirfieldElevationM)); // fixed for server/client parity (#486)
    REQUIRE(def.runways.size() == 1u);
    CHECK(def.runways[0].lengthM == Approx(2500.0));
    CHECK(def.runways[0].widthM == Approx(45.0));
    CHECK(def.runways[0].headingDeg == Approx(90.0));
    CHECK(def.runways[0].surface == RunwaySurface::Asphalt);
}

// ---------------------------------------------------------------------------
// OurAirports CSV import (#486)
// ---------------------------------------------------------------------------

TEST_CASE("importOurAirports parses airports and runways", "[airport]") {
    // Header + rows mirroring the real OurAirports schema (selective quoting, feet units).
    const std::string airports =
        "\"id\",\"ident\",\"type\",\"name\",\"latitude_deg\",\"longitude_deg\",\"elevation_ft\"\n"
        "1,\"KLAX\",\"large_airport\",\"Los Angeles Intl\",33.9425,-118.408,125\n"
        "2,\"CLOSD\",\"closed\",\"Closed Field\",10,20,100\n"
        "3,\"NOELEV\",\"small_airport\",\"No Elevation, Comma Name\",5,6,\n";
    const std::string runways =
        "\"id\",\"airport_ref\",\"airport_ident\",\"length_ft\",\"width_ft\",\"surface\",\"lighted\",\"closed\","
        "\"le_ident\",\"le_latitude_deg\",\"le_longitude_deg\",\"le_elevation_ft\",\"le_heading_degT\","
        "\"le_displaced_threshold_ft\",\"he_ident\",\"he_latitude_deg\",\"he_longitude_deg\",\"he_elevation_ft\","
        "\"he_heading_degT\",\"he_displaced_threshold_ft\"\n"
        "10,1,\"KLAX\",12000,150,\"CON\",\"1\",\"0\",\"07\",,,,70,,\"25\",,,,,\n"
        "11,1,\"KLAX\",8000,75,\"ASP\",\"1\",\"1\",\"06\",,,,,,,,,,,\n"     // closed runway -> skipped
        "12,3,\"NOELEV\",2000,,\"GRASS\",\"0\",\"0\",\"09\",,,,,,,,,,,,\n"; // no width, no heading

    AirportCsvStats stats;
    const std::vector<AirportDef> defs = importOurAirports(airports, runways, &stats);

    CHECK(stats.skippedClosed >= 2u); // the closed airport + the closed runway
    REQUIRE(defs.size() == 2u);       // KLAX + NOELEV (closed airport dropped)

    const AirportDef* klax = nullptr;
    const AirportDef* noelev = nullptr;
    for (const auto& d : defs) {
        if (d.id == "KLAX")
            klax = &d;
        if (d.id == "NOELEV")
            noelev = &d;
    }
    REQUIRE(klax != nullptr);
    REQUIRE(noelev != nullptr);
    // Feet -> metres.
    CHECK(klax->elevationM == Approx(125.0 * 0.3048));
    CHECK(klax->latRad == Approx(33.9425 * std::numbers::pi / 180.0));
    REQUIRE(klax->runways.size() == 1u); // the closed runway was skipped
    CHECK(klax->runways[0].lengthM == Approx(12000.0 * 0.3048));
    CHECK(klax->runways[0].widthM == Approx(150.0 * 0.3048));
    CHECK(klax->runways[0].surface == RunwaySurface::Concrete);
    CHECK(klax->runways[0].headingDeg == Approx(70.0));
    // NOELEV: missing elevation -> terrain-resolved; embedded comma in name survives; width default;
    // heading from the "09" ident (90 deg).
    CHECK(noelev->elevationM < 0.0);
    CHECK(noelev->name == "No Elevation, Comma Name");
    REQUIRE(noelev->runways.size() == 1u);
    CHECK(noelev->runways[0].widthM == Approx(30.0)); // default when width absent
    CHECK(noelev->runways[0].headingDeg == Approx(90.0));
}

TEST_CASE("runwaySurfaceFromOurAirports maps the common surface codes", "[airport]") {
    CHECK(runwaySurfaceFromOurAirports("CON") == RunwaySurface::Concrete);
    CHECK(runwaySurfaceFromOurAirports("conc") == RunwaySurface::Concrete);
    CHECK(runwaySurfaceFromOurAirports("PEM") == RunwaySurface::Concrete);
    CHECK(runwaySurfaceFromOurAirports("ASP") == RunwaySurface::Asphalt);
    CHECK(runwaySurfaceFromOurAirports("ASPH-G") == RunwaySurface::Asphalt);
    CHECK(runwaySurfaceFromOurAirports("TURF") == RunwaySurface::Grass);
    CHECK(runwaySurfaceFromOurAirports("GRASS") == RunwaySurface::Grass);
    CHECK(runwaySurfaceFromOurAirports("WATER") == RunwaySurface::Water);
    CHECK(runwaySurfaceFromOurAirports("GVL") == RunwaySurface::Gravel);
    CHECK(runwaySurfaceFromOurAirports("DIRT") == RunwaySurface::Gravel);
    CHECK(runwaySurfaceFromOurAirports("") == RunwaySurface::Gravel);    // blank -> unpaved default
    CHECK(runwaySurfaceFromOurAirports("UNK") == RunwaySurface::Gravel); // unknown -> unpaved default
}

// ---------------------------------------------------------------------------
// FLAB binary index (#486)
// ---------------------------------------------------------------------------

namespace {
std::vector<AirportDef> sampleDefs() {
    std::vector<AirportDef> defs;
    defs.push_back(geodeticDef("KLAX", 33.9425, -118.408, 38.0, 70.f, 3600.f));
    AirportDef strip;
    strip.id = "builtin:airfield";
    strip.name = "Sandbox";
    strip.useWorldXZ = true;
    strip.worldX = 4000.0;
    strip.worldZ = 0.0;
    strip.elevationM = -1.0;
    strip.acceptsLandings = true;
    strip.runways.push_back(RunwayDef{90.f, 2500.f, 45.f, RunwaySurface::Asphalt});
    defs.push_back(strip);
    return defs;
}
} // namespace

TEST_CASE("airport index round-trips and is byte-identical", "[airport]") {
    const std::vector<AirportDef> defs = sampleDefs();
    const uint64_t hash = airportSourceHash("airports-bytes", "runways-bytes");
    const std::vector<uint8_t> bytes = writeAirportIndex(defs, hash);

    auto decoded = readAirportIndex(bytes, hash);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == defs.size());
    for (std::size_t i = 0; i < defs.size(); ++i) {
        CHECK((*decoded)[i].id == defs[i].id);
        CHECK((*decoded)[i].name == defs[i].name);
        CHECK((*decoded)[i].latRad == Approx(defs[i].latRad));
        CHECK((*decoded)[i].lonRad == Approx(defs[i].lonRad));
        CHECK((*decoded)[i].elevationM == Approx(defs[i].elevationM));
        CHECK((*decoded)[i].useWorldXZ == defs[i].useWorldXZ);
        CHECK((*decoded)[i].worldX == Approx(defs[i].worldX));
        CHECK((*decoded)[i].acceptsLandings == defs[i].acceptsLandings);
        REQUIRE((*decoded)[i].runways.size() == defs[i].runways.size());
        CHECK((*decoded)[i].runways[0].surface == defs[i].runways[0].surface);
        CHECK((*decoded)[i].runways[0].lengthM == Approx(defs[i].runways[0].lengthM));
    }
    // Re-serializing the decoded defs yields the identical byte stream (determinism).
    CHECK(writeAirportIndex(*decoded, hash) == bytes);
}

TEST_CASE("airport index rejects mismatches and corruption", "[airport]") {
    const std::vector<AirportDef> defs = sampleDefs();
    const uint64_t hash = airportSourceHash("a", "b");
    const std::vector<uint8_t> bytes = writeAirportIndex(defs, hash);

    CHECK_FALSE(readAirportIndex(bytes, hash + 1).has_value());                       // source hash mismatch
    CHECK_FALSE(readAirportIndex(std::span(bytes).subspan(0, 10), hash).has_value()); // truncated
    std::vector<uint8_t> bad = bytes;
    bad[0] ^= 0xFF; // corrupt magic
    CHECK_FALSE(readAirportIndex(bad, hash).has_value());
}

// ---------------------------------------------------------------------------
// Spatial grid + flattening (#486)
// ---------------------------------------------------------------------------

TEST_CASE("airportsNear grid matches a brute-force scan", "[airport]") {
    std::vector<AirportDef> defs;
    // A scatter of airports incl. dateline and high-latitude cases.
    for (int i = 0; i < 200; ++i) {
        const double lat = -85.0 + (i * 170.0 / 199.0);
        const double lon = -179.0 + std::fmod(i * 53.0, 358.0);
        defs.push_back(geodeticDef("ap" + std::to_string(i), lat, lon, 100.0));
    }
    AirportRegistry reg;
    reg.load(std::move(defs), kEarthRadiusM, nullptr);

    auto brute = [&](LatLonAlt c, double radiusKm) {
        int n = 0;
        reg.forEach([&](const ResolvedAirport& a) {
            const LatLonAlt lla = worldToGeodetic(a.worldPos.x, a.worldPos.y, a.worldPos.z, kEarthRadiusM);
            const double dLat = lla.lat_rad - c.lat_rad;
            const double dLon = lla.lon_rad - c.lon_rad;
            const double s = std::sin(dLat * 0.5), t = std::sin(dLon * 0.5);
            const double h = s * s + std::cos(c.lat_rad) * std::cos(lla.lat_rad) * t * t;
            const double d = 2.0 * kEarthRadiusM * std::asin(std::min(1.0, std::sqrt(h)));
            if (d <= radiusKm * 1000.0)
                ++n;
        });
        return n;
    };

    for (auto [latDeg, lonDeg, rk] : std::initializer_list<std::tuple<double, double, double>>{
             {0.0, 0.0, 2000.0}, {40.0, -100.0, 1500.0}, {0.0, 179.5, 1000.0}, {80.0, 10.0, 3000.0}}) {
        const LatLonAlt c{latDeg * std::numbers::pi / 180.0, lonDeg * std::numbers::pi / 180.0, 0.0};
        std::vector<const ResolvedAirport*> got;
        reg.airportsNear(c, rk, got);
        CHECK(static_cast<int>(got.size()) == brute(c, rk));
    }
}

TEST_CASE("flattenedHeight is flat in the core and blends out", "[airport]") {
    // A single field at the origin-adjacent world-XZ position, elevation 500 m, 3 km E-W runway.
    AirportDef def;
    def.id = "flat";
    def.name = "Flat";
    def.useWorldXZ = true;
    def.worldX = 0.0;
    def.worldZ = 0.0;
    def.elevationM = 500.0;
    def.runways.push_back(RunwayDef{90.f, 3000.f, 60.f, RunwaySurface::Concrete});
    AirportRegistry reg;
    reg.load({def}, kEarthRadiusM, nullptr);
    const ResolvedAirport* a = reg.byId("flat");
    REQUIRE(a != nullptr);

    const glm::dvec3 c = a->worldPos;
    // At the field centre, the terrain flattens exactly to the field elevation regardless of raw.
    CHECK(reg.flattenedHeight(c, 123.0) == Approx(500.0));
    CHECK(reg.flattenedHeight(c, 800.0) == Approx(500.0));
    // Far away (200 km east along +X): untouched.
    const glm::dvec3 far{c.x + 200000.0, c.y, c.z};
    CHECK(reg.flattenedHeight(far, 250.0) == Approx(250.0));
    // In the blend annulus: core cross half-extent = W/2 + shoulder = 60 m; blend reaches 2x = 120 m.
    // 90 m is mid-annulus, so the flattened height sits strictly between raw (300) and field (500).
    const glm::dvec3 side{c.x, c.y, c.z + 90.0};
    const double blended = reg.flattenedHeight(side, 300.0);
    CHECK(blended > 300.0);
    CHECK(blended < 500.0);
}

TEST_CASE("runwaySurfaceAt and regionHasRunway", "[airport]") {
    AirportDef def;
    def.id = "surf";
    def.name = "Surf";
    def.useWorldXZ = true;
    def.worldX = 0.0;
    def.worldZ = 0.0;
    def.elevationM = 0.0;
    def.runways.push_back(RunwayDef{90.f, 2000.f, 45.f, RunwaySurface::Concrete});
    AirportRegistry reg;
    reg.load({def}, kEarthRadiusM, nullptr);
    const glm::dvec3 c = reg.byId("surf")->worldPos;

    auto surf = reg.runwaySurfaceAt(c);
    REQUIRE(surf.has_value());
    CHECK(*surf == RunwaySurface::Concrete);
    // A point 10 km away is on no runway.
    CHECK_FALSE(reg.runwaySurfaceAt(glm::dvec3{c.x + 10000.0, c.y, c.z}).has_value());
    // The tile bounding sphere at the field intersects the footprint; one far away does not.
    CHECK(reg.regionHasRunway(c, 500.0));
    CHECK_FALSE(reg.regionHasRunway(glm::dvec3{c.x + 100000.0, c.y, c.z}, 500.0));
}
