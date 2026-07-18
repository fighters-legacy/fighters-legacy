// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "world/AirportDef.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace fl {

// Binary airport cache ("FLAB", #486). The OurAirports CSV import is not cheap to run every launch
// (~80k airports, ~48k runways); this serializes the parsed AirportDefs into a compact,
// byte-deterministic index written once to the per-user cache dir and re-read on subsequent launches.
//
// The format is explicit little-endian byte writes (never a struct memcpy — determinism across
// compilers/platforms), versioned, and stamped with a source hash so a CSV change invalidates it. The
// spatial grid is NOT serialized — it is rebuilt in AirportRegistry::load(), keeping the format small.

// FNV-1a over the airports.csv bytes then the runways.csv bytes. A mismatch means the source data
// changed and the cache must be rebuilt.
[[nodiscard]] uint64_t airportSourceHash(std::string_view airportsCsv, std::string_view runwaysCsv) noexcept;

// Serialize defs into the FLAB v1 byte stream. Identical defs + sourceHash produce identical bytes on
// every platform.
[[nodiscard]] std::vector<uint8_t> writeAirportIndex(const std::vector<AirportDef>& defs, uint64_t sourceHash);

// Parse a FLAB byte stream. Returns nullopt on a bad magic/version, a sourceHash mismatch, or a
// truncated/malformed buffer (so a stale or corrupt cache falls back to a fresh import).
[[nodiscard]] std::optional<std::vector<AirportDef>> readAirportIndex(std::span<const uint8_t> bytes,
                                                                      uint64_t expectedSourceHash);

} // namespace fl
