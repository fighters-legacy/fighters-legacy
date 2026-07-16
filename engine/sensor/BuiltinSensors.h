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

    // The builtin semi-active radar seeker head (#862) — BuiltinWeapon::sarhMissile(),
    // `sensor_id = "builtin:sarh-seeker"`. Unlike the active-radar seeker it is a PASSIVE receiver
    // (`emitter = false`): it homes on energy the SHOOTER's radar reflects off the target, so it never
    // announces itself and never appears on an RWR the way an active seeker does. The shot depends on
    // the launch platform holding its lock (the ProjectileSystem SupportQuery / illumination path).
    [[nodiscard]] static const SensorDef& sarhSeeker() {
        static const SensorDef s = [] {
            SensorDef d;
            d.id = "builtin:sarh-seeker";
            d.name = "Semi-Active Radar Seeker";
            d.type = SensorType::Radar;
            d.omnidirectional = false;
            d.emitter = false; // passive: rides the shooter's illumination, never radiates

            d.search.azHalfAngleDeg = 20.f;
            d.search.elHalfAngleDeg = 20.f;
            d.search.minRangeM = 150.f;
            d.search.maxRangeM = 24076.f; // 13 nm off a baseline (rcs 1.0) fighter under illumination
            d.search.pod = 0.5f;

            SensorLobe track;
            track.azHalfAngleDeg = 25.f;
            track.elHalfAngleDeg = 25.f;
            track.minRangeM = 100.f;
            track.maxRangeM = 29632.f; // 16 nm
            track.pod = 0.85f;
            d.track = track;
            d.lockHoldS = 1.5f;

            return d;
        }();
        return s;
    }

    // The builtin ground air-defense search radar (#863) — the SAM site's eyes, `sensor_id =
    // "builtin:sam-radar"`. A long-range EMITTER (the RWR/EMCON seam #529 reads it as "a SAM is
    // painting me"), omnidirectional in azimuth so a fixed emplacement covers the whole sky, not just
    // a boresight cone. Radar range scales by sqrt(rcs) like every radar in the one-vocabulary model.
    [[nodiscard]] static const SensorDef& groundRadar() {
        static const SensorDef s = [] {
            SensorDef d;
            d.id = "builtin:sam-radar";
            d.name = "SAM Search Radar";
            d.type = SensorType::Radar;
            d.omnidirectional = true; // a ground battery watches the whole hemisphere
            d.emitter = true;

            d.search.azHalfAngleDeg = 180.f; // omni: the lobe angles are nominal
            d.search.elHalfAngleDeg = 85.f;
            d.search.minRangeM = 500.f;
            d.search.maxRangeM = 55560.f; // 30 nm against a baseline (rcs 1.0) fighter
            d.search.pod = 0.5f;

            SensorLobe track;
            track.azHalfAngleDeg = 180.f;
            track.elHalfAngleDeg = 85.f;
            track.minRangeM = 300.f;
            track.maxRangeM = 64820.f; // 35 nm
            track.pod = 0.85f;
            d.track = track;
            d.lockHoldS = 3.0f;

            return d;
        }();
        return s;
    }
};

} // namespace fl::sensor
