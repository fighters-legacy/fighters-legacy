// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/AeroForces.h" // PayloadEffect

#include <string>
#include <vector>

namespace fl {

struct EntityDef;
struct FlightModelData;
class WeaponRegistry;

namespace sensor {
struct SensorDef;
}

// One line of the manual: a label and the value the aircraft actually has.
struct ManualRow {
    std::string label;
    std::string value;
};

struct ManualSection {
    std::string title;
    std::vector<ManualRow> rows;
};

// A pilot's reference for one aircraft (#821).
//
// EVERY NUMBER IN HERE IS GENERATED, NOT AUTHORED. That is the entire design.
//
// The tempting alternative -- hand-write a manual page per aircraft -- duplicates every number the
// flight model already contains, and drifts from it within a month: the first time someone retunes
// the drag polar, the manual silently starts lying and nothing in CI notices. So the manual is a
// CONSUMER OF THE SAME SOURCES OF TRUTH AS THE SIMULATION: performance from fl::trim() (the same
// function fm-trim's CI gate uses, #817), limits from [aero.limits], stations from
// EntityDef::hardpoints, stores from the WeaponRegistry (#812), sensors from the entity's sensor ids
// (#810). It therefore CANNOT disagree with the aircraft you are flying.
//
// The only thing a content author writes is prose -- what the aircraft is, how it flies, what it is
// good and bad at -- and that part contains no numbers.
struct AircraftManual {
    std::string title; // "F-5E Tiger II"
    std::vector<ManualSection> sections;
    std::vector<std::string> prose; // the pack's hand-written notes, verbatim; may be empty
};

// Inputs the caller has already resolved. Sensors are passed as resolved defs rather than ids so
// engine-manual does not need the AssetManager: the server resolved them at spawn, and the client
// has them from the same content pack.
struct ManualSources {
    const EntityDef* entity{nullptr};
    const FlightModelData* model{nullptr};
    const WeaponRegistry* weapons{nullptr};        // null = no loadout section
    std::vector<const sensor::SensorDef*> sensors; // empty = no sensor section
    PayloadEffect payload{};                       // the default loadout, from fl::defaultPayload
    std::string prose;                             // raw text from the pack; may be empty
};

// Builds the manual. Trims the model at three standard conditions (sea level, 15 000 ft, and the
// tropopause) at clean and combat weight.
//
// Called ONCE when the manual is opened, and cached. Deliberately NOT called per frame: trim is a
// root-finding loop, and it has no business in the render path.
[[nodiscard]] AircraftManual buildAircraftManual(const ManualSources& src);

} // namespace fl
