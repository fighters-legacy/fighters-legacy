// SPDX-License-Identifier: GPL-3.0-or-later
//
// loadOrImportAirports: the CSV-or-cache decision (#1145).
//
// fl-server and the game client both call this and must derive the BYTE-IDENTICAL airport set,
// because the runway-flatten parity between them depends on it. That makes the cache the risk: a
// stale index that is accepted anyway gives the server one set of runways and the client another,
// and the aircraft touches down through terrain that is flat on one machine and not the other.
//
// So the cases here are about which branch is taken and whether the result matches — cache hit,
// cache miss, stale hash, corrupt file, unwritable cache directory — rather than about the import
// itself, which test_airport_csv_import.cpp covers.

#include <catch2/catch_test_macros.hpp>

#include "mock_hal.h"
#include "world/AirportBootstrap.h"
#include "world/AirportIndexFile.h"

#include <string>
#include <vector>

using namespace fl;

namespace {

constexpr const char* kAirportsCsv = "data/airports.csv";
constexpr const char* kRunwaysCsv = "data/runways.csv";
constexpr const char* kCachePath = "cache/airports.flab";

const char* kAirports = "id,ident,type,name,latitude_deg,longitude_deg,elevation_ft\n"
                        "1,KLAX,large_airport,Los Angeles Intl,33.9425,-118.408,125\n"
                        "2,KDEN,large_airport,Denver Intl,39.8617,-104.6731,5433\n";
const char* kRunways = "id,airport_ident,length_ft,width_ft,surface,closed,le_ident\n"
                       "10,KLAX,12000,150,CON,0,07\n"
                       "11,KDEN,16000,200,CON,0,16R\n";

void addCsvs(MockFilesystem& fs) {
    fs.addFile(kAirportsCsv, kAirports);
    fs.addFile(kRunwaysCsv, kRunways);
}

} // namespace

TEST_CASE("loadOrImportAirports: absent CSVs are not an error (#1145)", "[airport][bootstrap]") {
    // The database is optional content. Without it the builtin airfield and any pack airports still
    // load, so this has to be an empty list rather than a failure that takes the world down.
    MockFilesystem fs;
    MockLogger log;
    AirportLoadStats stats;

    CHECK(loadOrImportAirports(fs, log, &stats).empty());
    CHECK_FALSE(stats.csvPresent);
    CHECK_FALSE(stats.cacheHit);
    CHECK(stats.airports == 0u);

    // Half the pair is the same as none: importing airports with no runways would produce a
    // different set than the machine that has both.
    fs.addFile(kAirportsCsv, kAirports);
    CHECK(loadOrImportAirports(fs, log, &stats).empty());
    CHECK_FALSE(stats.csvPresent);
}

TEST_CASE("loadOrImportAirports: a cold start imports the CSVs and writes the cache (#1145)", "[airport][bootstrap]") {
    MockFilesystem fs;
    MockLogger log;
    addCsvs(fs);

    AirportLoadStats stats;
    const std::vector<AirportDef> defs = loadOrImportAirports(fs, log, &stats);

    CHECK(stats.csvPresent);
    CHECK_FALSE(stats.cacheHit);
    CHECK(stats.airports == 2u);
    REQUIRE(defs.size() == 2u);
    CHECK(fs.files.count(kCachePath) == 1u); // the cache was written for next time
}

TEST_CASE("loadOrImportAirports: the second start reads the cache and gets the same set (#1145)",
          "[airport][bootstrap]") {
    // The whole point of the cache. If the decoded set differed from the imported one in any way,
    // a client that had run before and a server that had not would disagree about the runways.
    MockFilesystem fs;
    MockLogger log;
    addCsvs(fs);

    AirportLoadStats first;
    const std::vector<AirportDef> imported = loadOrImportAirports(fs, log, &first);
    REQUIRE_FALSE(first.cacheHit);

    AirportLoadStats second;
    const std::vector<AirportDef> cached = loadOrImportAirports(fs, log, &second);
    CHECK(second.cacheHit);
    CHECK(second.airports == first.airports);

    REQUIRE(cached.size() == imported.size());
    for (std::size_t i = 0; i < cached.size(); ++i) {
        INFO("airport " << i);
        CHECK(cached[i].id == imported[i].id);
        CHECK(cached[i].name == imported[i].name);
        CHECK(cached[i].latRad == imported[i].latRad);
        CHECK(cached[i].lonRad == imported[i].lonRad);
        CHECK(cached[i].elevationM == imported[i].elevationM);
        REQUIRE(cached[i].runways.size() == imported[i].runways.size());
        for (std::size_t r = 0; r < cached[i].runways.size(); ++r) {
            CHECK(cached[i].runways[r].lengthM == imported[i].runways[r].lengthM);
            CHECK(cached[i].runways[r].widthM == imported[i].runways[r].widthM);
            CHECK(cached[i].runways[r].headingDeg == imported[i].runways[r].headingDeg);
            CHECK(cached[i].runways[r].surface == imported[i].runways[r].surface);
        }
    }
}

TEST_CASE("loadOrImportAirports: a cache built from different CSVs is refused (#1145)", "[airport][bootstrap]") {
    // The source hash is what makes shipping an updated database safe. Accepting a stale index would
    // leave a player who has run the old build flying to airports that no longer exist there.
    MockFilesystem fs;
    MockLogger log;
    addCsvs(fs);

    AirportLoadStats warm;
    (void)loadOrImportAirports(fs, log, &warm); // priming the cache; the defs are checked elsewhere
    REQUIRE(fs.files.count(kCachePath) == 1u);

    // Ship a new database: one more airport, so the hash changes.
    fs.files.erase(kAirportsCsv);
    fs.addFile(kAirportsCsv, std::string(kAirports) + "3,KSFO,large_airport,San Francisco Intl,37.6188,-122.375,13\n");

    AirportLoadStats stats;
    const std::vector<AirportDef> defs = loadOrImportAirports(fs, log, &stats);
    CHECK_FALSE(stats.cacheHit); // the stale index was rejected...
    CHECK(stats.airports == 3u); // ...and the new database imported
    CHECK(defs.size() == 3u);

    // And the rewritten cache is now the current one.
    AirportLoadStats again;
    (void)loadOrImportAirports(fs, log, &again);
    CHECK(again.cacheHit);
    CHECK(again.airports == 3u);
}

TEST_CASE("loadOrImportAirports: a corrupt or truncated cache falls back to the CSVs (#1145)", "[airport][bootstrap]") {
    // A cache half-written when the machine lost power. Losing the airport database over it would be
    // the wrong trade — re-importing costs a second.
    MockFilesystem fs;
    MockLogger log;
    addCsvs(fs);

    SECTION("garbage bytes") {
        fs.addFile(kCachePath, "this is not an airport index");
        AirportLoadStats stats;
        CHECK(loadOrImportAirports(fs, log, &stats).size() == 2u);
        CHECK_FALSE(stats.cacheHit);
    }
    SECTION("empty file") {
        fs.addFile(kCachePath, "");
        AirportLoadStats stats;
        CHECK(loadOrImportAirports(fs, log, &stats).size() == 2u);
        CHECK_FALSE(stats.cacheHit);
    }
    SECTION("a valid index truncated mid-record") {
        AirportLoadStats warm;
        (void)loadOrImportAirports(fs, log, &warm); // priming the cache so it can be corrupted
        auto& bytes = fs.files[kCachePath];
        REQUIRE(bytes.size() > 16u);
        bytes.resize(bytes.size() / 2);

        AirportLoadStats stats;
        CHECK(loadOrImportAirports(fs, log, &stats).size() == 2u);
        CHECK_FALSE(stats.cacheHit);
    }
}

TEST_CASE("loadOrImportAirports: a cache that cannot be written still returns the airports (#1145)",
          "[airport][bootstrap]") {
    // A read-only profile directory, or a full disk. The player loses the startup speed-up, not the
    // airports — and the warning says which.
    MockFilesystem fs;
    MockLogger log;
    addCsvs(fs);
    fs.failWriteOpen = true;

    AirportLoadStats stats;
    const std::vector<AirportDef> defs = loadOrImportAirports(fs, log, &stats);
    CHECK(defs.size() == 2u);
    CHECK_FALSE(stats.cacheHit);
    CHECK(log.hasMessage(LogLevel::Warn, "cache"));
    CHECK(fs.files.count(kCachePath) == 0u);

    // Every subsequent start re-imports rather than silently serving nothing.
    AirportLoadStats again;
    CHECK(loadOrImportAirports(fs, log, &again).size() == 2u);
    CHECK_FALSE(again.cacheHit);
}

TEST_CASE("loadOrImportAirports: the stats pointer is optional (#1145)", "[airport][bootstrap]") {
    MockFilesystem fs;
    MockLogger log;
    addCsvs(fs);
    CHECK(loadOrImportAirports(fs, log).size() == 2u);          // cold
    CHECK(loadOrImportAirports(fs, log, nullptr).size() == 2u); // warm
}

TEST_CASE("airportSourceHash: either CSV changing changes the hash (#1145)", "[airport][bootstrap]") {
    // Hashing only the airports file would let a runway-only update through the cache — and runway
    // geometry is exactly what the flatten parity depends on.
    const uint64_t base = airportSourceHash(kAirports, kRunways);
    CHECK(airportSourceHash(kAirports, kRunways) == base); // stable
    CHECK(airportSourceHash(std::string(kAirports) + "3,KSFO,small_airport,SFO,37.6,-122.3,13\n", kRunways) != base);
    CHECK(airportSourceHash(kAirports, std::string(kRunways) + "12,KLAX,9000,100,ASP,0,25\n") != base);
    CHECK(airportSourceHash("", "") != base);
}
