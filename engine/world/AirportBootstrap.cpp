// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/AirportBootstrap.h"

#include "IFilesystem.h"
#include "ILogger.h"
#include "util/FsRead.h"
#include "world/AirportCsvImport.h"
#include "world/AirportIndexFile.h"

#include <cstdint>
#include <optional>
#include <string>

namespace fl {

namespace {

constexpr const char* kAirportsCsv = "data/airports.csv";
constexpr const char* kRunwaysCsv = "data/runways.csv";
constexpr const char* kCacheDir = "cache";
constexpr const char* kCachePath = "cache/airports.flab";

void writeCache(IFilesystem& fs, ILogger& log, const std::vector<uint8_t>& bytes) {
    fs.createDirectory(PathDomain::UserData, kCacheDir);
    const int h = fs.openFile(PathDomain::UserData, kCachePath, /*write=*/true);
    if (h < 0) {
        log.log(LogLevel::Warn, __FILE__, __LINE__, "airports: could not open cache for writing");
        return;
    }
    fs.writeFile(h, bytes.data(), bytes.size());
    fs.closeFile(h);
}

} // namespace

std::vector<AirportDef> loadOrImportAirports(IFilesystem& fs, ILogger& log, AirportLoadStats* stats) {
    AirportLoadStats st;

    auto airportsCsv = readFileToString(fs, PathDomain::Assets, kAirportsCsv);
    auto runwaysCsv = readFileToString(fs, PathDomain::Assets, kRunwaysCsv);
    if (!airportsCsv || !runwaysCsv) {
        if (stats)
            *stats = st; // csvPresent = false
        return {};
    }
    st.csvPresent = true;
    const uint64_t hash = airportSourceHash(*airportsCsv, *runwaysCsv);

    // Try the cached binary index first.
    if (auto cached = readFileBytes(fs, PathDomain::UserData, kCachePath)) {
        if (auto defs = readAirportIndex(*cached, hash)) {
            st.airports = defs->size();
            st.cacheHit = true;
            if (stats)
                *stats = st;
            return std::move(*defs);
        }
    }

    // Miss / stale / corrupt: re-import the CSVs and rewrite the cache.
    AirportCsvStats importStats;
    std::vector<AirportDef> defs = importOurAirports(*airportsCsv, *runwaysCsv, &importStats);
    writeCache(fs, log, writeAirportIndex(defs, hash));
    st.airports = defs.size();
    st.cacheHit = false;
    if (stats)
        *stats = st;
    return defs;
}

} // namespace fl
