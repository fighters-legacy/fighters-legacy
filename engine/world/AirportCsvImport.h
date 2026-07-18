// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "world/AirportDef.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace fl {

// Counts from an OurAirports import, for a summary log.
struct AirportCsvStats {
    uint32_t airports{0};      // airports emitted (open, with valid coordinates)
    uint32_t runways{0};       // runways attached
    uint32_t skippedClosed{0}; // closed airports/runways skipped
    uint32_t badRows{0};       // rows dropped for a parse/validation failure
};

// Parse the bundled OurAirports database (airports.csv + runways.csv, RFC-4180) into AirportDefs.
// Airports of type "closed" and runways with closed == 1 are skipped; feet convert to metres; a
// runway heading falls back from le_heading_degT to the endpoint bearing to the runway-ident number;
// the free-text surface string maps to a RunwaySurface by prefix. An airport with no valid
// coordinates is dropped. `stats` (optional) receives the counts.
[[nodiscard]] std::vector<AirportDef> importOurAirports(std::string_view airportsCsv, std::string_view runwaysCsv,
                                                        AirportCsvStats* stats = nullptr);

// Map an OurAirports free-text surface string (any case) to a RunwaySurface by prefix. Exposed for
// unit testing the surface table. Unknown/blank -> Gravel (an unpaved default; never a hard error).
[[nodiscard]] RunwaySurface runwaySurfaceFromOurAirports(std::string_view surface) noexcept;

} // namespace fl
