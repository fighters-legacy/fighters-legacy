// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace fl::sensor {

// What physical channel a sensor observes. The four types differ in their PARAMETERS, not in their
// model: a radar, an IRST, a laser designator and a human eyeball are all "a cone, a range band, and
// a probability of seeing something in it" (2026-07-12 decision record, docs/developer/architecture.md).
//
// The type selects which of an entity's signature multipliers the detection math reads, and whether
// the emissions kernel applies: radar and laser TRACK lobes require the observer to be emitting.
enum class SensorType : uint8_t { Visual, Ir, Radar, Laser };

// How a sensor is CONSUMED, not how it works — the detection math treats both identically. It exists
// so tooling can apply the right emitter rule: an aircraft (or SAM) radar must EMIT to hold a lock,
// so a non-emitting radar/laser track lobe is an authoring mistake. A weapon SEEKER head can be a
// passive receiver — a semi-active radar seeker rides the SHOOTER's illumination and holds a lock
// while transmitting nothing (builtin:sarh-seeker), so the same "emitter = false + track lobe"
// shape is correct there. Defaults to Aircraft; a pack marks a seeker head `role = "seeker"`.
enum class SensorRole : uint8_t { Aircraft, Seeker };

// One detection lobe. A sensor has a SEARCH lobe (wide, low probability — how a target is found)
// and optionally a TRACK lobe (narrow, high probability — how it is held).
//
// `pod` is the probability of detection PER CHECK at the reference cadence of 10 Hz
// ([world] sensor_check_hz). A PoD without a rate is meaningless — the same 0.3 is a different
// sensor at 1 Hz than at 60 Hz — so authors tune against the reference and an operator who changes
// the cadence changes effective acquisition time. That is the honest consequence, and it is not
// silently renormalized.
struct SensorLobe {
    float azHalfAngleDeg{0.f}; // (0, 180]; 180 = no azimuth limit
    float elHalfAngleDeg{0.f}; // (0, 180]; 90 = no elevation limit
    float minRangeM{0.f};      // dead zone; 0 = none
    float maxRangeM{0.f};      // range against a baseline (signature 1.0) target
    float pod{0.f};            // (0, 1] per check @ 10 Hz
};

// Immutable definition for one sensor, loaded from a content pack TOML file (sensors/*.toml).
//
// THE POINT OF THIS TYPE is that there is only one of it. Player avionics (#526), AI detection
// (#670) and missile seekers (#628/#676) were each about to grow their own idea of "what can this
// thing see"; they now read the same def. A consumer that wants to know whether something is
// visible asks the sensor system, and gets back a Contact — never a ground-truth position.
//
// RANGES ARE STORED IN SI (metres, seconds), authored in nautical miles. Same rule as weapon defs:
// the source data and the people writing it use aviation units, and the parser converts on the way
// in, so nothing downstream has to remember which field was authored in what.
struct SensorDef {
    std::string id; // content-pack-scoped, e.g. "fl-base:apg63"
    std::string name;
    SensorType type{SensorType::Visual};

    // No cone at all: the sensor sees in every direction (an RWR, a missile-approach warner). The
    // lobe's half-angles still exist and still describe a full sphere, so the detection math has no
    // special case — this flag is what an AUTHOR writes instead of remembering that 180/90 means
    // "everywhere".
    bool omnidirectional{false};

    // The sensor announces itself when it looks (radar, laser designator). Radar and laser TRACK
    // lobes require the observer to be emitting. This is the seam RWR, EMCON and SAM radar shutdown
    // hang off (#526/#529); nothing consumes it yet, and landing it now costs one bool.
    bool emitter{false};

    // Consumer role (see SensorRole). Only tooling reads it — a Seeker head is exempt from the
    // "a non-emitting radar/laser track lobe can never lock" plausibility warning, because a passive
    // SARH seeker legitimately has exactly that shape.
    SensorRole role{SensorRole::Aircraft};

    SensorLobe search;               // required: how a target is found
    std::optional<SensorLobe> track; // absent = search-only (an eyeball cannot hold a lock)
    float lockHoldS{0.f};            // track coast time after the target leaves the cone/fails PoD

    // ECCM — resistance to ECM/noise jamming (#529), [0, 1], 0 = none. A jamming target denies this
    // radar a LOCK beyond a burn-through range = track.maxRange × (kEcmBaseBurnThrough + eccm),
    // clamped to 1. A high-ECCM radar burns through farther out; a poor one must close right in. Only
    // meaningful for a radar with a track lobe — a passive sensor is not jammed by noise on the radar
    // band. Authored `[sensor] eccm`.
    float eccm{0.f};
};

} // namespace fl::sensor
