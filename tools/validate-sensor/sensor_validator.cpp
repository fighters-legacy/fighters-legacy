// SPDX-License-Identifier: GPL-3.0-or-later
#include "sensor_validator.h"

#include "math/Units.h"

#include "sensor/SensorDef.h"
#include "sensor/SensorDefParser.h"

#include <exception>
#include <string>

namespace fl {

namespace {

using sensor::SensorDef;
using sensor::SensorRole;
using sensor::SensorType;

// Plausibility bounds. NOT schema limits — everything here parses and runs. They mark values so far
// outside a real sensor that they are more likely a typo or a unit mix-up than an intent.
constexpr float kImplausibleRangeM = 400.f * fl::kMetresPerNauticalMile<float>; // beyond any airborne sensor
constexpr float kImplausibleVisualRangeM = 25.f * fl::kMetresPerNauticalMile<float>;
constexpr float kGlacialPod = 0.001f; // @ 10 Hz: ~100 s to acquire, i.e. never in a merge

[[nodiscard]] bool isPassive(SensorType t) {
    return t == SensorType::Visual || t == SensorType::Ir;
}

void checkPlausibility(const SensorDef& s, SensorValidationResult& r) {
    // A passive sensor that announces itself is a contradiction: an eyeball and an IRST are how you
    // look at something WITHOUT being seen looking. The emitter flag drives RWR and EMCON (#526/
    // #529), so this one would produce a wrong warning on the receiving end, not just a wrong def.
    if (s.emitter && isPassive(s.type))
        r.warnings.push_back("emitter = true on a passive sensor (visual/ir) — passive sensors do "
                             "not radiate, and this will light up an RWR that should stay quiet");

    // Radar and laser TRACK lobes require the observer to be emitting (the emissions kernel). A
    // non-emitting radar with a track lobe carries a lobe it can never use — UNLESS it is a weapon
    // SEEKER head (role = "seeker"), where a passive semi-active seeker legitimately rides the
    // shooter's illumination and holds a lock while transmitting nothing (builtin:sarh-seeker). The
    // rule is written for aircraft/SAM radars; exempt seekers so a correctly-authored SARH def is
    // clean while an aircraft radar with the same shape still warns (#902).
    if (!s.emitter && s.track && s.role != SensorRole::Seeker &&
        (s.type == SensorType::Radar || s.type == SensorType::Laser))
        r.warnings.push_back("a radar/laser [track] lobe requires emitter = true — as authored, "
                             "this sensor can never hold a lock");

    if (s.search.maxRangeM > kImplausibleRangeM)
        r.warnings.push_back("search.max_range_nm is implausibly long for an airborne sensor — "
                             "check the units (nautical miles, not metres)");

    if (s.type == SensorType::Visual && s.search.maxRangeM > kImplausibleVisualRangeM)
        r.warnings.push_back("search.max_range_nm is implausibly long for a visual sensor — an "
                             "unaided eye does not find a fighter at this range");

    // ECCM extends an ECM burn-through range, and burn-through is evaluated in exactly one place:
    // a RADAR deciding whether it may hold a LOCK on a jamming target (SensorSystem). Authored on
    // anything else the value parses, stores, and does nothing — the same shape as #1105, where an
    // eccm the parser never read looked authored and was inert. Say so rather than let it read as
    // tuning.
    if (s.eccm > 0.f && (s.type != SensorType::Radar || !s.track))
        r.warnings.push_back("eccm > 0 on a sensor that can never use it — burn-through applies "
                             "only to a radar holding a track lock, so this value is inert");

    if (s.search.pod < kGlacialPod)
        r.warnings.push_back("search.pod is so low the sensor will effectively never acquire "
                             "(PoD is per check at 10 Hz, not per second)");

    if (s.track) {
        const auto& t = *s.track;

        // The search lobe is how a target is FOUND and the track lobe is how it is HELD. A track
        // lobe that reaches further, sees wider, or detects more reliably than the search lobe that
        // must feed it describes a sensor that can hold what it can never find.
        if (t.maxRangeM > s.search.maxRangeM)
            r.warnings.push_back("track.max_range_nm exceeds search.max_range_nm — a target cannot "
                                 "be tracked before it has been found");
        if (t.azHalfAngleDeg > s.search.azHalfAngleDeg || t.elHalfAngleDeg > s.search.elHalfAngleDeg)
            r.warnings.push_back("the [track] lobe is wider than the [search] lobe — a track lobe "
                                 "is normally the narrower, higher-confidence one");
        if (t.pod < s.search.pod)
            r.warnings.push_back("track.pod is below search.pod — tracking is normally more "
                                 "reliable than searching, not less");

        if (s.lockHoldS <= 0.f)
            r.warnings.push_back("track.lock_hold_s is 0 — the track drops the instant a single "
                                 "check fails, so locks will flicker");
    }
}

} // namespace

SensorValidationResult validateSensor(std::string_view tomlContent) {
    SensorValidationResult r;

    SensorDef def;
    try {
        def = sensor::parseSensorDef(tomlContent);
    } catch (const std::exception& e) {
        r.ok = false;
        r.errors.emplace_back(e.what());
        return r;
    }

    checkPlausibility(def, r);
    return r;
}

} // namespace fl
