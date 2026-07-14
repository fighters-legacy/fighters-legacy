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

    // The builtin IR seeker head (#440) — referenced by BuiltinWeapon::irMissile() via
    // `sensor_id = "builtin:ir-seeker"`. A heat source against sky is conspicuous once inside the
    // gimbal cone, so acquisition PoD is high and the track lobe is nearly certain; the constraint
    // is the narrow cone and the modest range, not the dice. Passive — an IR seeker emits nothing.
    [[nodiscard]] static const SensorDef& irSeeker() {
        static const SensorDef s = [] {
            SensorDef d;
            d.id = "builtin:ir-seeker";
            d.name = "IR Seeker";
            d.type = SensorType::Ir;
            d.omnidirectional = false;
            d.emitter = false;

            d.search.azHalfAngleDeg = 25.f; // the acquisition basket off boresight
            d.search.elHalfAngleDeg = 25.f;
            d.search.minRangeM = 0.f;
            d.search.maxRangeM = 9260.f; // 5 nm against a baseline (ir 1.0) tailpipe
            d.search.pod = 0.55f;

            SensorLobe track;
            track.azHalfAngleDeg = 35.f; // gimbal limit once locked
            track.elHalfAngleDeg = 35.f;
            track.minRangeM = 0.f;
            track.maxRangeM = 11112.f; // 6 nm
            track.pod = 0.9f;
            d.track = track;
            d.lockHoldS = 1.0f; // brief memory through a flare/occlusion blink

            return d;
        }();
        return s;
    }

    // The builtin active-radar seeker head (#440) — BuiltinWeapon::radarMissile(),
    // `sensor_id = "builtin:radar-seeker"`. An EMITTER: when the missile goes active at pitbull it
    // starts announcing itself, which is the seam the RWR (#529) hangs off. Radar range scales by
    // sqrt(rcs) like every radar in the one-vocabulary model.
    [[nodiscard]] static const SensorDef& radarSeeker() {
        static const SensorDef s = [] {
            SensorDef d;
            d.id = "builtin:radar-seeker";
            d.name = "Active Radar Seeker";
            d.type = SensorType::Radar;
            d.omnidirectional = false;
            d.emitter = true;

            d.search.azHalfAngleDeg = 25.f;
            d.search.elHalfAngleDeg = 25.f;
            d.search.minRangeM = 300.f;   // a radar fuze blind zone, not a gun's
            d.search.maxRangeM = 22224.f; // 12 nm against a baseline (rcs 1.0) fighter
            d.search.pod = 0.4f;

            SensorLobe track;
            track.azHalfAngleDeg = 30.f;
            track.elHalfAngleDeg = 30.f;
            track.minRangeM = 150.f;
            track.maxRangeM = 27780.f; // 15 nm
            track.pod = 0.85f;
            d.track = track;
            d.lockHoldS = 2.0f; // coast through a notch before going dumb

            return d;
        }();
        return s;
    }
};

} // namespace fl::sensor
