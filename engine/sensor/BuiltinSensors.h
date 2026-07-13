// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sensor/SensorDef.h"

namespace fl::sensor {

// The compiled-in eyeball, mirroring BuiltinFlightModel / BuiltinWeapon.
//
// This is not a convenience: it is what makes honest sensing the DEFAULT rather than an opt-in
// feature flag (2026-07-12 decision record). An AI-controlled entity that declares no `sensors` list
// gets this, so there is no configuration of the engine — including the zero-content-pack sandbox —
// in which an AI sees through terrain or across the map, and no content pack can produce one by
// omission.
//
// Deliberately unremarkable: a pilot's forward scan. Wide but not spherical, moderate range against
// a baseline fighter, low per-check probability (spotting a fighter-sized target unaided takes a
// while), and NO track lobe — an eyeball finds an aircraft, it does not hold a lock on one.
struct BuiltinSensors {
    [[nodiscard]] static const SensorDef& eyeball() {
        static const SensorDef s = [] {
            SensorDef d;
            d.id = "builtin:eyeball";
            d.name = "Visual Acquisition";
            d.type = SensorType::Visual;
            d.omnidirectional = false;
            d.emitter = false;

            d.search.azHalfAngleDeg = 90.f; // ±90°: what a pilot can scan without turning the jet
            d.search.elHalfAngleDeg = 60.f;
            d.search.minRangeM = 0.f;
            d.search.maxRangeM = 14816.f; // 8 nm against a baseline fighter in clear air
            d.search.pod = 0.15f;         // per check @ 10 Hz — a few seconds to acquire

            return d;
        }();
        return s;
    }
};

} // namespace fl::sensor
