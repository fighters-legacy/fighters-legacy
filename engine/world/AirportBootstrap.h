// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "world/AirportDef.h"

#include <vector>

namespace fl {

class IFilesystem;
class ILogger;

// Loads the bundled OurAirports database (#486), using a cached binary index when it is current.
//
// Reads `data/airports.csv` + `data/runways.csv` from PathDomain::Assets, hashes them, and tries
// `cache/airports.flab` from PathDomain::UserData: a hit (matching source hash) is decoded directly;
// a miss/stale/corrupt cache re-imports the CSVs and rewrites the cache. Returns the CSV airport defs
// (empty when the CSVs are absent — the builtin airfield + pack airports still load). fl-server and
// the game client both call this so they derive the byte-identical airport set the runway-flatten
// parity depends on. Main thread (file I/O). `stats` (optional out) receives {airports, cacheHit}.
struct AirportLoadStats {
    std::size_t airports{0};
    bool cacheHit{false};
    bool csvPresent{false};
};
[[nodiscard]] std::vector<AirportDef> loadOrImportAirports(IFilesystem& fs, ILogger& log,
                                                           AirportLoadStats* stats = nullptr);

} // namespace fl
