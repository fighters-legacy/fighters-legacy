// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/SignatureDef.h"
#include "sensor/SensorDef.h"

#include <cstdint>

namespace fl::sensor {

// Pure detection math: given an observer pose, a sensor, a target position + signature, an
// environment and a deterministic seed, decide what the observer can see. No tick machinery, no
// storage, no I/O — `SensorSystem` (#685) owns all of that and calls into here.
//
// ─────────────────────────────────────────────────────────────────────────────
// THE CENTRAL RULE: PoD GATES ACQUISITION, GEOMETRY MAINTAINS THE CONTACT.
//
// A probability of detection is the chance of *finding* something you were not already looking at.
// It is NOT re-rolled to keep a contact you already hold: a target already inside the cone stays
// held without a roll, and is lost when it leaves the cone (or when its coast expires), not when a
// die comes up short. Rolling every check to *retain* a contact would make a 0.35-PoD radar drop
// and re-acquire the same untouched target several times a second — a flicker that is neither
// physical nor playable, and that no amount of downstream smoothing can honestly repair.
//
// So: `stepContact` needs a roll only on the Lost→Detected and Detected→Locked edges.
// ─────────────────────────────────────────────────────────────────────────────

// Weather / time-of-day inputs to detection (#209 fills the curves the seam was landed for).
struct SensingEnvironment {
    float cloudCoverage{0.f}; // [0, 1]
    float fogDensity{0.f};    // [0, 1]
    float fogStartDist{0.f};  // metres
    float timeOfDayH{12.f};   // hours [0, 24)
    bool isNight{false};      // true = the visual channel is degraded (curve lands in #209)
};

// What one sensor sees of one target this check. Geometry and dice are reported SEPARATELY,
// because the state machine needs them separately (see the central rule above).
struct SensorEvaluation {
    bool searchInLobe{false};   // inside the search cone + range band
    bool trackInLobe{false};    // inside the track cone + range band (false when search-only)
    bool searchRollPass{false}; // the acquisition die for the search lobe
    bool trackRollPass{false};  // the acquisition die for the track lobe
};

// A contact's lifecycle. `Coasting` is a first-class state, not a bug: a lock that has lost its
// target coasts on last-known state for the sensor's lock_hold_s before it drops.
enum class ContactState : uint8_t {
    Lost,     // not held
    Detected, // held by the search lobe — bearing and range, no firing-quality track
    Locked,   // held by the track lobe
    Coasting, // was held, geometry lost, running out lock_hold_s on last-known state
};

// The per-(observer, target, sensor) state the machine carries between checks. `SensorSystem` owns
// storage; this struct is passed in and returned by value, so the machine itself is pure.
struct ContactTrack {
    ContactState state{ContactState::Lost};
    uint64_t firstDetectedTick{0}; // tick of the Lost→Detected edge; drives reaction delay (#685)
    uint64_t lastSeenTick{0};      // last tick the target was actually inside a lobe
    float coastRemainingS{0.f};    // > 0 only while Coasting
};

// ── signature and range ──────────────────────────────────────────────────────

// The target's signature multiplier for the channel this sensor observes.
[[nodiscard]] float signatureMultiplier(SensorType type, const SignatureDef& sig) noexcept;

// Range at which this lobe can detect THIS target, as opposed to the baseline (signature 1.0)
// target its `max_range_nm` is quoted against.
//
// Radar scales by `sqrt(sig)`; IR, visual and laser scale linearly (2026-07-12 decision record).
// The square root echoes the fourth-power range dependence of the radar equation closely enough to
// make stealth feel right without dragging a radar equation into a content pack.
//
// `radarRangeFraction` is the difficulty scaling applied to RADAR sensors only (fl-server passes
// `AiScaling::radarSensorRange`; 1.0 = no reduction). It is taken as a plain float rather than an
// `AiScaling` so this stays pure math with no dependency on the config layer.
[[nodiscard]] float effectiveMaxRangeM(const SensorLobe& lobe, SensorType type, const SignatureDef& sig,
                                       float radarRangeFraction = 1.f) noexcept;

// ── geometry ─────────────────────────────────────────────────────────────────

// Is the target inside this lobe's cone and range band?
//
// The cone is tested in the observer's BODY frame (forward = +X, up = +Y, right = +Z — the
// convention `fl::ai::bodyForward` and every AI controller already use): azimuth is the angle off
// the nose in the body's horizontal plane, elevation the angle above it. `omnidirectional` skips
// the cone test entirely — an RWR has nothing to point.
//
// `effMaxRangeM` is what `effectiveMaxRangeM` returned; `lobe.minRangeM` is the dead zone, and a
// target inside it is NOT detected (that hole is deliberate — it is how a missile-guidance radar
// loses a target that flies down its throat).
[[nodiscard]] bool inLobe(const double observerPos[3], const float observerQuat[4], const double targetPos[3],
                          const SensorLobe& lobe, bool omnidirectional, float effMaxRangeM) noexcept;

// ── probability ──────────────────────────────────────────────────────────────

// The lobe's authored PoD, scaled by the observer's crew skill and the environment.
//
// `skill` is `EntityDef [ai].skill` in [0, 1] and maps to a multiplier of `0.5 + skill`, so the
// DEFAULT skill of 0.5 is exactly unity: an entity that authors no `[ai]` section detects at
// precisely the probability the sensor def states. That invariant is why the mapping is affine and
// not, say, a curve through the origin.
//
// The result is clamped to (0, 1].
[[nodiscard]] float effectivePod(float authoredPod, float skill, SensorType type,
                                 const SensingEnvironment& env) noexcept;

// How much harder the weather and the dark make this KIND of sensor's job (#209). A multiplier on
// probability of detection, in (0, 1] — 1.0 = the conditions cost nothing.
//
// A DEFAULT-CONSTRUCTED SensingEnvironment (clear, noon) RETURNS EXACTLY 1.0. That is load-bearing:
// every authored `pod` is quoted against clear daylight, so fair weather must cost nothing at all
// rather than "almost nothing" — otherwise every content pack's numbers would quietly mean something
// slightly different than they say.
//
// The four channels degrade differently, because they are different physics and the point of having
// four of them is that a pilot can reach for the one the weather has not taken away:
//
//   * VISUAL is hit hardest, and is the only one the DARK touches. Cloud and fog take it; night takes
//     most of what is left. An eyeball in a storm at 2 a.m. is very nearly useless — which is why
//     night attacks work, and why an IR-equipped aircraft owns that night.
//   * IR is degraded by moisture (cloud, fog attenuate in the infrared) but NOT by darkness. It does
//     not care what time it is: it is looking at heat, and a jet engine is exactly as hot at midnight.
//   * RADAR barely notices any of it — a little rain clutter, nothing more. That indifference to
//     weather is most of why it exists.
//   * LASER tracks the visual channel through moisture (same line-of-sight problem) but, like IR,
//     does not care about the dark.
//
// A floor keeps every channel from reaching exactly zero: even in the worst conditions a sensor has
// SOME chance, so a target is never mathematically undetectable — it just may take a very long time.
[[nodiscard]] float environmentPodScale(SensorType type, const SensingEnvironment& env) noexcept;

// Deterministic acquisition die for one (observer, target, tick, sensor slot, lobe) check.
//
// Seeded exactly like the per-entity turbulence RNG in `WorldBroadcaster::stepFlightSim`, and for
// the same reason: no shared RNG state is mutated across entities, so the result is independent of
// evaluation ORDER and identical across worker counts, platforms and replays. A sensing pass that
// gave different contacts on a 4-worker server than on a 1-worker one would be a desync generator.
//
// `lobeSalt` separates the search and track rolls of the same sensor on the same tick, so a lucky
// search roll does not imply a lucky track roll.
[[nodiscard]] uint32_t detectionHash(uint32_t observerIdx, uint32_t targetIdx, uint64_t tickIndex, uint32_t sensorSlot,
                                     uint32_t lobeSalt) noexcept;

// Does this hash pass the given probability? The comparison is INTEGER (the hash against a 24-bit
// threshold derived from `pod`), so there is no float-ordering ambiguity across compilers — the
// determinism guarantee above is only worth as much as this comparison.
[[nodiscard]] bool rollPasses(uint32_t hash, float pod) noexcept;

// ── one sensor against one target ────────────────────────────────────────────

// Evaluates both lobes of one sensor against one target: geometry and dice, no state.
//
// `emitting` is the observer's emissions flag for THIS sensor. Both lobes of a radar or laser require
// it — a radar sees nothing it does not first illuminate, so an emitter switched off (EMCON / radar
// Silent mode, #526) detects NOTHING, search and track alike. Passive sensors (IR, visual) ignore the
// flag entirely: they receive, they do not radiate. (This is the EMCON gate the emissions kernel was
// landed for — before #526 the search lobe of a non-emitting radar still detected, a passive-radar
// free lunch that never made sense.)
//
// `allowTrack` gates the TRACK lobe on top of everything else: false suppresses locking regardless of
// geometry, which is how radar Search mode reports bearing without ever offering a firing solution
// (#526). The default is true so seekers and passive sensors are unaffected.
[[nodiscard]] SensorEvaluation evaluateSensor(const SensorDef& sensor, bool emitting, const double observerPos[3],
                                              const float observerQuat[4], const double targetPos[3],
                                              const SignatureDef& targetSig, float skill, const SensingEnvironment& env,
                                              float radarRangeFraction, uint32_t observerIdx, uint32_t targetIdx,
                                              uint64_t tickIndex, uint32_t sensorSlot, bool allowTrack = true) noexcept;

// ── the contact state machine ────────────────────────────────────────────────

// Advances one contact by one CHECK. Pure: state in, state out.
//
// - Lost → Detected requires the target in the search lobe AND the search die.
// - Detected/Locked/Coasting stay held while the target is in the search lobe — NO die (the central
//   rule). Re-entering the search lobe also recovers a Coasting contact without a new roll: it was
//   never lost, only unobserved.
// - Detected → Locked requires the target in the track lobe AND the track die. An already-Locked
//   contact stays locked while it remains in the track lobe, again with no die.
// - Losing the search lobe starts the coast: `Coasting` for `lockHoldS` seconds, then `Lost`. A
//   search-only sensor (an eyeball) has `lockHoldS == 0` and therefore drops the moment the target
//   leaves its cone — which is exactly right: you have not "lost track" of something you were only
//   ever looking at, you have simply stopped seeing it.
//
// `dtS` is the wall-time since the previous check for THIS contact (the sensing cadence, not the
// sim step), so the coast runs out in real seconds regardless of `sensor_check_hz`.
[[nodiscard]] ContactTrack stepContact(const ContactTrack& prev, const SensorEvaluation& eval, float lockHoldS,
                                       float dtS, uint64_t tickIndex) noexcept;

} // namespace fl::sensor
