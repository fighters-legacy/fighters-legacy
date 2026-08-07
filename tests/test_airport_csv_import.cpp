// SPDX-License-Identifier: GPL-3.0-or-later
//
// importOurAirports against a hostile CSV (#1145). test_airport_registry.cpp covers the happy path
// with a well-formed extract; this file covers what the real 80,000-row OurAirports database
// actually contains — truncated rows, blank coordinates, runways pointing at airports that were
// dropped, quoted fields with commas and quotes and newlines inside them, and every branch of the
// three-step heading fallback.
//
// The stakes are specific: this import runs once and produces the airport list the whole world uses.
// A row that parses WRONG is worse than a row that is dropped, because a runway at the wrong heading
// is a runway an AI will line up on and miss.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "world/AirportCsvImport.h"

#include <numbers>
#include <string>

using Catch::Approx;
using namespace fl;

namespace {

constexpr const char* kAirportHeader = "id,ident,type,name,latitude_deg,longitude_deg,elevation_ft\n";
constexpr const char* kRunwayHeader = "id,airport_ident,length_ft,width_ft,surface,closed,le_ident,"
                                      "le_latitude_deg,le_longitude_deg,le_heading_degT,"
                                      "he_latitude_deg,he_longitude_deg\n";

const AirportDef* find(const std::vector<AirportDef>& defs, std::string_view id) {
    for (const auto& d : defs)
        if (d.id == id)
            return &d;
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Degenerate input
// ---------------------------------------------------------------------------

TEST_CASE("importOurAirports: empty input yields nothing rather than crashing (#1145)", "[airport][csv]") {
    AirportCsvStats stats;
    CHECK(importOurAirports("", "", &stats).empty());
    CHECK(stats.airports == 0u);
    CHECK(importOurAirports("", "", nullptr).empty()); // the stats pointer is optional
}

TEST_CASE("importOurAirports: a header with no data rows yields no airports (#1145)", "[airport][csv]") {
    AirportCsvStats stats;
    const auto defs = importOurAirports(kAirportHeader, kRunwayHeader, &stats);
    CHECK(defs.empty());
    CHECK(stats.airports == 0u);
    CHECK(stats.runways == 0u);
}

TEST_CASE("importOurAirports: airports without a runways file still import (#1145)", "[airport][csv]") {
    // The runway half is genuinely optional — an airport with no runway data is still a place on the
    // map, and losing all 80,000 of them because one file was missing would be the worse failure.
    const std::string airports = std::string(kAirportHeader) + "1,KLAX,large_airport,LAX,33.9,-118.4,125\n";
    AirportCsvStats stats;
    const auto defs = importOurAirports(airports, "", &stats);
    REQUIRE(defs.size() == 1u);
    CHECK(defs[0].runways.empty());
    CHECK(stats.airports == 1u);
}

// ---------------------------------------------------------------------------
// Rows the real database contains
// ---------------------------------------------------------------------------

TEST_CASE("importOurAirports: a row missing its coordinates is dropped and counted (#1145)", "[airport][csv]") {
    // OurAirports has heliports and seaplane bases with blank lat/lon. Emitting one at 0,0 would put
    // an airport in the Gulf of Guinea, which is exactly the kind of wrong that looks like a bug in
    // the terrain system three months later.
    const std::string airports = std::string(kAirportHeader) +
                                 "1,NOLAT,small_airport,No Latitude,,-118.4,125\n"
                                 "2,NOLON,small_airport,No Longitude,33.9,,125\n"
                                 "3,,small_airport,No Ident,33.9,-118.4,125\n" // blank ident
                                 "4,BADNUM,small_airport,Not A Number,abc,-118.4,125\n"
                                 "5,GOOD,small_airport,Good,33.9,-118.4,125\n";
    AirportCsvStats stats;
    const auto defs = importOurAirports(airports, "", &stats);

    REQUIRE(defs.size() == 1u);
    CHECK(defs[0].id == "GOOD");
    CHECK(stats.badRows == 4u);
}

TEST_CASE("importOurAirports: a truncated row is read as blank cells, not out of bounds (#1145)", "[airport][csv]") {
    // Rows shorter than the header appear in the wild. Reading past the end would be UB; the cell
    // lookup has to answer "blank" for a column the row simply does not have.
    const std::string airports = std::string(kAirportHeader) + "1,SHORT,small_airport\n" // stops before lat/lon
                                 + "2,GOOD,small_airport,Good,33.9,-118.4,125\n";
    AirportCsvStats stats;
    const auto defs = importOurAirports(airports, "", &stats);
    REQUIRE(defs.size() == 1u);
    CHECK(defs[0].id == "GOOD");
    CHECK(stats.badRows == 1u);
}

TEST_CASE("importOurAirports: a nameless airport is named by its ident (#1145)", "[airport][csv]") {
    const std::string airports = std::string(kAirportHeader) + "1,KXYZ,small_airport,,33.9,-118.4,125\n";
    const auto defs = importOurAirports(airports, "");
    REQUIRE(defs.size() == 1u);
    CHECK(defs[0].name == "KXYZ"); // a blank label on a map is worse than a terse one
}

TEST_CASE("importOurAirports: an unparseable elevation means terrain-resolved, not sea level (#1145)",
          "[airport][csv]") {
    // -1 is the sentinel for "ask the terrain". Zero would place a mountain airfield at sea level
    // and bury the runway.
    const std::string airports = std::string(kAirportHeader) + "1,KXYZ,small_airport,X,33.9,-118.4,n/a\n";
    const auto defs = importOurAirports(airports, "");
    REQUIRE(defs.size() == 1u);
    CHECK(defs[0].elevationM < 0.0f);
}

// ---------------------------------------------------------------------------
// The CSV reader itself
// ---------------------------------------------------------------------------

TEST_CASE("importOurAirports: quoted fields carry commas, quotes and newlines (#1145)", "[airport][csv]") {
    // RFC-4180 escaping. "" inside a quoted field is one literal quote; a newline inside quotes does
    // NOT end the row. Getting either wrong shifts every subsequent column by one.
    const std::string airports = std::string(kAirportHeader) +
                                 "1,KQTE,small_airport,\"Bob's \"\"Field\"\", Ltd\",33.9,-118.4,125\n"
                                 "2,KNL,small_airport,\"Two\nLines\",34.0,-118.0,100\n";
    const auto defs = importOurAirports(airports, "");

    REQUIRE(defs.size() == 2u);
    const AirportDef* quoted = find(defs, "KQTE");
    REQUIRE(quoted != nullptr);
    CHECK(quoted->name == "Bob's \"Field\", Ltd");
    const AirportDef* multiline = find(defs, "KNL");
    REQUIRE(multiline != nullptr);
    CHECK(multiline->name == "Two\nLines");
}

TEST_CASE("importOurAirports: CRLF line endings parse the same as LF (#1145)", "[airport][csv]") {
    // The published files are CRLF. A stray \r landing in the ident would make every runway's
    // airport_ident lookup miss and silently produce runway-less airports.
    const std::string airports = "id,ident,type,name,latitude_deg,longitude_deg,elevation_ft\r\n"
                                 "1,KLAX,large_airport,LAX,33.9,-118.4,125\r\n";
    const std::string runways = "id,airport_ident,length_ft,width_ft,surface,closed,le_ident\r\n"
                                "10,KLAX,12000,150,CON,0,07\r\n";
    AirportCsvStats stats;
    const auto defs = importOurAirports(airports, runways, &stats);

    REQUIRE(defs.size() == 1u);
    CHECK(defs[0].id == "KLAX");
    REQUIRE(defs[0].runways.size() == 1u); // the ident matched across both files
    CHECK(stats.runways == 1u);
}

TEST_CASE("importOurAirports: a file with no trailing newline keeps its last row (#1145)", "[airport][csv]") {
    const std::string airports = std::string(kAirportHeader) + "1,KLAST,small_airport,Last,33.9,-118.4,125";
    CHECK(importOurAirports(airports, "").size() == 1u);
}

// ---------------------------------------------------------------------------
// Runway attachment and validation
// ---------------------------------------------------------------------------

TEST_CASE("importOurAirports: a runway for an unknown airport is discarded quietly (#1145)", "[airport][csv]") {
    // Every runway of a closed airport lands here. It is expected, not an error, so it must not
    // inflate badRows — an operator watching that number needs it to mean something.
    const std::string airports = std::string(kAirportHeader) + "1,KLAX,large_airport,LAX,33.9,-118.4,125\n"
                                                               "2,GONE,closed,Closed Field,10,20,100\n";
    const std::string runways = std::string(kRunwayHeader) + "10,GONE,9000,100,ASP,0,09,,,,,\n"
                                                             "11,NEVEREXISTED,9000,100,ASP,0,09,,,,,\n";
    AirportCsvStats stats;
    const auto defs = importOurAirports(airports, runways, &stats);

    REQUIRE(defs.size() == 1u);
    CHECK(defs[0].runways.empty());
    CHECK(stats.badRows == 0u);
    CHECK(stats.skippedClosed == 1u); // the airport only; the orphan runways are not "closed"
}

TEST_CASE("importOurAirports: a runway with no usable length is dropped (#1145)", "[airport][csv]") {
    // A zero-length runway would divide by zero in the approach geometry, and a blank one is just
    // missing data. Both are dropped rather than defaulted, because guessing a runway length is
    // guessing whether an aircraft can land there.
    const std::string airports = std::string(kAirportHeader) + "1,KLAX,large_airport,LAX,33.9,-118.4,125\n";
    const std::string runways = std::string(kRunwayHeader) + "10,KLAX,0,100,ASP,0,09,,,,,\n"
                                                             "11,KLAX,,100,ASP,0,09,,,,,\n"
                                                             "12,KLAX,-500,100,ASP,0,09,,,,,\n"
                                                             "13,KLAX,9000,100,ASP,0,09,,,,,\n";
    AirportCsvStats stats;
    const auto defs = importOurAirports(airports, runways, &stats);

    REQUIRE(defs.size() == 1u);
    CHECK(defs[0].runways.size() == 1u);
    CHECK(stats.badRows == 3u);
    CHECK(stats.runways == 1u);
}

TEST_CASE("importOurAirports: a missing or zero width falls back to 30 m (#1145)", "[airport][csv]") {
    const std::string airports = std::string(kAirportHeader) + "1,KLAX,large_airport,LAX,33.9,-118.4,125\n";
    const std::string runways = std::string(kRunwayHeader) + "10,KLAX,9000,,ASP,0,09,,,,,\n"
                                                             "11,KLAX,9000,0,ASP,0,09,,,,,\n"
                                                             "12,KLAX,9000,-5,ASP,0,09,,,,,\n";
    const auto defs = importOurAirports(airports, runways);
    REQUIRE(defs.size() == 1u);
    REQUIRE(defs[0].runways.size() == 3u);
    for (const auto& rw : defs[0].runways)
        CHECK(rw.widthM == Approx(30.0f));
}

// ---------------------------------------------------------------------------
// The heading fallback chain
// ---------------------------------------------------------------------------

TEST_CASE("importOurAirports: the heading falls back through all three sources (#1145)", "[airport][csv]") {
    // le_heading_degT -> bearing between the two thresholds -> the ident number x10 -> 0. Each rung
    // exists because the row above it is blank somewhere in the real database.
    const std::string airports = std::string(kAirportHeader) + "1,A,small_airport,A,0,0,0\n";

    SECTION("the published true heading wins") {
        const std::string runways = std::string(kRunwayHeader) + "10,A,9000,100,ASP,0,09,0,0,123.5,1,1\n";
        const auto defs = importOurAirports(airports, runways);
        REQUIRE(defs[0].runways.size() == 1u);
        CHECK(defs[0].runways[0].headingDeg == Approx(123.5f));
    }
    SECTION("no heading: the bearing between thresholds") {
        // From (0,0) due east to (0,1): a bearing of 90 degrees.
        const std::string runways = std::string(kRunwayHeader) + "10,A,9000,100,ASP,0,09,0,0,,0,1\n";
        const auto defs = importOurAirports(airports, runways);
        REQUIRE(defs[0].runways.size() == 1u);
        CHECK(defs[0].runways[0].headingDeg == Approx(90.0f).margin(0.01));
    }
    SECTION("no heading and no endpoints: the runway ident") {
        const std::string runways = std::string(kRunwayHeader) + "10,A,9000,100,ASP,0,27L,,,,,\n";
        const auto defs = importOurAirports(airports, runways);
        REQUIRE(defs[0].runways.size() == 1u);
        CHECK(defs[0].runways[0].headingDeg == Approx(270.0f));
    }
    SECTION("only one endpoint is not enough for a bearing") {
        // A half-specified pair must fall THROUGH to the ident, not compute a bearing to (0,0).
        const std::string runways = std::string(kRunwayHeader) + "10,A,9000,100,ASP,0,18,0,0,,,\n";
        const auto defs = importOurAirports(airports, runways);
        REQUIRE(defs[0].runways.size() == 1u);
        CHECK(defs[0].runways[0].headingDeg == Approx(180.0f));
    }
    SECTION("nothing at all: zero, and the runway is still kept") {
        const std::string runways = std::string(kRunwayHeader) + "10,A,9000,100,ASP,0,,,,,,\n";
        const auto defs = importOurAirports(airports, runways);
        REQUIRE(defs[0].runways.size() == 1u);
        CHECK(defs[0].runways[0].headingDeg == Approx(0.0f));
    }
}

TEST_CASE("importOurAirports: an ident that is not a compass number is refused (#1145)", "[airport][csv]") {
    // Runway idents run 01..36. "H1" (a helipad), "00" and "99" are not headings, and multiplying
    // them by ten would aim an approach at nothing.
    const std::string airports = std::string(kAirportHeader) + "1,A,small_airport,A,0,0,0\n";
    const std::string runways = std::string(kRunwayHeader) + "10,A,9000,100,ASP,0,H1,,,,,\n"
                                                             "11,A,9000,100,ASP,0,00,,,,,\n"
                                                             "12,A,9000,100,ASP,0,99,,,,,\n"
                                                             "13,A,9000,100,ASP,0,ALL,,,,,\n";
    const auto defs = importOurAirports(airports, runways);
    REQUIRE(defs[0].runways.size() == 4u);
    for (const auto& rw : defs[0].runways)
        CHECK(rw.headingDeg == Approx(0.0f));
}

TEST_CASE("importOurAirports: a bearing westward wraps into 0..360 (#1145)", "[airport][csv]") {
    // atan2 returns negatives for westward runs; a heading of -90 would fail every downstream
    // comparison that assumes a compass value.
    const std::string airports = std::string(kAirportHeader) + "1,A,small_airport,A,0,0,0\n";
    const std::string runways = std::string(kRunwayHeader) + "10,A,9000,100,ASP,0,27,0,0,,0,-1\n";
    const auto defs = importOurAirports(airports, runways);
    REQUIRE(defs[0].runways.size() == 1u);
    CHECK(defs[0].runways[0].headingDeg == Approx(270.0f).margin(0.01));
}

// ---------------------------------------------------------------------------
// The surface table
// ---------------------------------------------------------------------------

TEST_CASE("runwaySurfaceFromOurAirports: every prefix in the table is reachable (#1145)", "[airport][csv]") {
    // These strings are free text typed by thousands of contributors. Each prefix here is one that
    // appears in the published file; an unmatched one silently becomes gravel, which changes the
    // braking model under an aircraft that is landing.
    CHECK(runwaySurfaceFromOurAirports("CEM") == RunwaySurface::Concrete);
    CHECK(runwaySurfaceFromOurAirports("BIT") == RunwaySurface::Asphalt); // bitumen
    CHECK(runwaySurfaceFromOurAirports("PAV") == RunwaySurface::Asphalt); // "paved"
    CHECK(runwaySurfaceFromOurAirports("TAR") == RunwaySurface::Asphalt); // tarmac
    CHECK(runwaySurfaceFromOurAirports("GRS") == RunwaySurface::Grass);
    CHECK(runwaySurfaceFromOurAirports("GRE") == RunwaySurface::Grass); // "green"
    CHECK(runwaySurfaceFromOurAirports("SOD") == RunwaySurface::Grass);
    CHECK(runwaySurfaceFromOurAirports("G") == RunwaySurface::Grass); // exact match only
    CHECK(runwaySurfaceFromOurAirports("WAT") == RunwaySurface::Water);
    CHECK(runwaySurfaceFromOurAirports("water") == RunwaySurface::Water); // case-insensitive

    // "G" is grass only as a whole cell. "GVL" starts with G and is gravel, so a prefix test would
    // have been wrong here.
    CHECK(runwaySurfaceFromOurAirports("GVL") == RunwaySurface::Gravel);
    CHECK(runwaySurfaceFromOurAirports("SAND") == RunwaySurface::Gravel);
    CHECK(runwaySurfaceFromOurAirports("CORAL") == RunwaySurface::Gravel);
}

TEST_CASE("importOurAirports: the surface string reaches the runway (#1145)", "[airport][csv]") {
    const std::string airports = std::string(kAirportHeader) + "1,A,small_airport,A,0,0,0\n";
    const std::string runways = std::string(kRunwayHeader) + "10,A,9000,100,\"ASPH-CONC\",0,09,,,,,\n"
                                                             "11,A,4000,60,turf,0,18,,,,,\n";
    const auto defs = importOurAirports(airports, runways);
    REQUIRE(defs[0].runways.size() == 2u);
    CHECK(defs[0].runways[0].surface == RunwaySurface::Asphalt);
    CHECK(defs[0].runways[1].surface == RunwaySurface::Grass);
}

// ---------------------------------------------------------------------------
// Units
// ---------------------------------------------------------------------------

TEST_CASE("importOurAirports: degrees become radians and feet become metres (#1145)", "[airport][csv]") {
    // The CSV is in degrees and feet; the engine is in radians and metres. One missed conversion
    // here misplaces every airport on Earth.
    const std::string airports = std::string(kAirportHeader) + "1,KDEN,large_airport,Denver,39.8617,-104.6731,5433\n";
    const std::string runways = std::string(kRunwayHeader) + "10,KDEN,16000,200,CON,0,16R,,,160,,\n";
    const auto defs = importOurAirports(airports, runways);

    REQUIRE(defs.size() == 1u);
    constexpr double kDegToRad = std::numbers::pi / 180.0;
    CHECK(defs[0].latRad == Approx(39.8617 * kDegToRad));
    CHECK(defs[0].lonRad == Approx(-104.6731 * kDegToRad));
    CHECK(defs[0].elevationM == Approx(5433.0 * 0.3048).epsilon(1e-4));
    REQUIRE(defs[0].runways.size() == 1u);
    CHECK(defs[0].runways[0].lengthM == Approx(16000.0f * 0.3048f).epsilon(1e-4));
    CHECK(defs[0].runways[0].widthM == Approx(200.0f * 0.3048f).epsilon(1e-4));
}
