// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/AirportBootstrap.h"

#include "IFilesystem.h"
#include "ILogger.h"
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

// Read an entire file from a domain into a string. Returns nullopt if it cannot be opened.
[[nodiscard]] std::optional<std::string> readAll(IFilesystem& fs, PathDomain domain, const char* path) {
    if (!fs.fileExists(domain, path))
        return std::nullopt;
    const int h = fs.openFile(domain, path, false);
    if (h < 0)
        return std::nullopt;
    const std::size_t size = fs.getFileSize(h);
    std::string out;
    out.resize(size);
    fs.readFile(h, out.data(), size);
    fs.closeFile(h);
    return out;
}

[[nodiscard]] std::optional<std::vector<uint8_t>> readAllBytes(IFilesystem& fs, PathDomain domain, const char* path) {
    if (!fs.fileExists(domain, path))
        return std::nullopt;
    const int h = fs.openFile(domain, path, false);
    if (h < 0)
        return std::nullopt;
    const std::size_t size = fs.getFileSize(h);
    std::vector<uint8_t> out(size);
    fs.readFile(h, out.data(), size);
    fs.closeFile(h);
    return out;
}

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

    auto airportsCsv = readAll(fs, PathDomain::Assets, kAirportsCsv);
    auto runwaysCsv = readAll(fs, PathDomain::Assets, kRunwaysCsv);
    if (!airportsCsv || !runwaysCsv) {
        if (stats)
            *stats = st; // csvPresent = false
        return {};
    }
    st.csvPresent = true;
    const uint64_t hash = airportSourceHash(*airportsCsv, *runwaysCsv);

    // Try the cached binary index first.
    if (auto cached = readAllBytes(fs, PathDomain::UserData, kCachePath)) {
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
