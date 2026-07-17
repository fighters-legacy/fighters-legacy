// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "sensor/BuiltinSensors.h"
#include "sensor/Detection.h"
#include "sensor/Iff.h"

#include <cmath>
#include <numbers>
#include <vector>

using namespace fl;
using namespace fl::sensor;
using Catch::Matchers::WithinRel;

namespace {

constexpr float kMPerNm = 1852.f;

// Identity attitude: nose along +X, up +Y, right +Z.
constexpr float kIdentityQuat[4] = {0.f, 0.f, 0.f, 1.f};

// Yaw about +Y by `deg`. Ry(-90°) points the nose at +Z (right); Ry(+90°) points it at -Z.
std::vector<float> yawQuat(float deg) {
    const float half = deg * (std::numbers::pi_v<float> / 180.f) * 0.5f;
    return {0.f, std::sin(half), 0.f, std::cos(half)}; // [x, y, z, w]
}

SensorLobe lobe(float az, float el, float minNm, float maxNm, float pod) {
    return SensorLobe{az, el, minNm * kMPerNm, maxNm * kMPerNm, pod};
}

// A radar with both lobes: 60/30 search out to 40 nm, 30/20 track out to 30 nm.
SensorDef radar(bool emitter = true) {
    SensorDef s;
    s.id = "t:radar";
    s.name = "Radar";
    s.type = SensorType::Radar;
    s.emitter = emitter;
    s.search = lobe(60.f, 30.f, 0.f, 40.f, 1.0f); // pod = 1 so geometry, not dice, is under test
    s.track = lobe(30.f, 20.f, 0.f, 30.f, 1.0f);
    s.lockHoldS = 4.f;
    return s;
}

const SignatureDef kBaseline{}; // all 1.0

// A target `metres` straight ahead of an identity-attitude observer at the origin.
std::vector<double> ahead(double metres) {
    return {metres, 0.0, 0.0};
}

} // namespace

// ── signature and range ──────────────────────────────────────────────────────

TEST_CASE("effectiveMaxRangeM: radar scales by sqrt(rcs), other channels linearly", "[sensor]") {
    const SensorLobe l = lobe(60.f, 30.f, 0.f, 40.f, 0.35f);

    SignatureDef stealth;
    stealth.rcs = 0.01f;   // a hundredth of a baseline fighter
    stealth.ir = 0.01f;    //
    stealth.visual = 0.5f; //

    // sqrt(0.01) = 0.1 — a tenth of the baseline range, not a hundredth. That is the whole point of
    // the square root: it echoes the fourth-power radar equation without dragging one in.
    CHECK_THAT(effectiveMaxRangeM(l, SensorType::Radar, stealth), WithinRel(0.1f * 40.f * kMPerNm, 1e-5f));

    // IR and visual are linear.
    CHECK_THAT(effectiveMaxRangeM(l, SensorType::Ir, stealth), WithinRel(0.01f * 40.f * kMPerNm, 1e-5f));
    CHECK_THAT(effectiveMaxRangeM(l, SensorType::Visual, stealth), WithinRel(0.5f * 40.f * kMPerNm, 1e-5f));

    // A baseline target sees exactly the authored range on every channel.
    CHECK_THAT(effectiveMaxRangeM(l, SensorType::Radar, kBaseline), WithinRel(40.f * kMPerNm, 1e-5f));
    CHECK_THAT(effectiveMaxRangeM(l, SensorType::Ir, kBaseline), WithinRel(40.f * kMPerNm, 1e-5f));
}

TEST_CASE("effectiveMaxRangeM: the difficulty fraction applies to radar only", "[sensor]") {
    const SensorLobe l = lobe(60.f, 30.f, 0.f, 40.f, 0.35f);

    CHECK_THAT(effectiveMaxRangeM(l, SensorType::Radar, kBaseline, 0.5f), WithinRel(20.f * kMPerNm, 1e-5f));
    CHECK_THAT(effectiveMaxRangeM(l, SensorType::Ir, kBaseline, 0.5f), WithinRel(40.f * kMPerNm, 1e-5f));
    CHECK_THAT(effectiveMaxRangeM(l, SensorType::Visual, kBaseline, 0.5f), WithinRel(40.f * kMPerNm, 1e-5f));
}

// ── geometry ─────────────────────────────────────────────────────────────────

TEST_CASE("inLobe: range band, including the min-range dead zone", "[sensor]") {
    const SensorLobe l = lobe(60.f, 30.f, 0.5f, 40.f, 1.f);
    const double obs[3] = {0, 0, 0};
    const float maxR = effectiveMaxRangeM(l, SensorType::Radar, kBaseline);

    CHECK(inLobe(obs, kIdentityQuat, ahead(20.0 * kMPerNm).data(), l, false, maxR));

    // Beyond max: gone.
    CHECK_FALSE(inLobe(obs, kIdentityQuat, ahead(41.0 * kMPerNm).data(), l, false, maxR));

    // Inside the dead zone: also gone. The hole is deliberate — it is how a target that flies down
    // the radar's throat is lost.
    CHECK_FALSE(inLobe(obs, kIdentityQuat, ahead(0.2 * kMPerNm).data(), l, false, maxR));
    CHECK(inLobe(obs, kIdentityQuat, ahead(0.6 * kMPerNm).data(), l, false, maxR));
}

TEST_CASE("inLobe: azimuth and elevation half-angles, boundary inclusive", "[sensor]") {
    const SensorLobe l = lobe(60.f, 30.f, 0.f, 40.f, 1.f);
    const double obs[3] = {0, 0, 0};
    const float maxR = effectiveMaxRangeM(l, SensorType::Radar, kBaseline);
    const double r = 10.0 * kMPerNm;

    auto atAzEl = [&](double azDeg, double elDeg) {
        const double az = azDeg * std::numbers::pi / 180.0;
        const double el = elDeg * std::numbers::pi / 180.0;
        // +Z is RIGHT, so a positive azimuth swings toward +Z.
        const double horiz = r * std::cos(el);
        return std::vector<double>{horiz * std::cos(az), r * std::sin(el), horiz * std::sin(az)};
    };

    CHECK(inLobe(obs, kIdentityQuat, atAzEl(0, 0).data(), l, false, maxR));     // dead ahead
    CHECK(inLobe(obs, kIdentityQuat, atAzEl(59.9, 0).data(), l, false, maxR));  // just inside
    CHECK(inLobe(obs, kIdentityQuat, atAzEl(-59.9, 0).data(), l, false, maxR)); // symmetric
    CHECK(inLobe(obs, kIdentityQuat, atAzEl(60.0, 0).data(), l, false, maxR));  // ON the boundary
    CHECK_FALSE(inLobe(obs, kIdentityQuat, atAzEl(60.5, 0).data(), l, false, maxR));
    CHECK_FALSE(inLobe(obs, kIdentityQuat, atAzEl(180, 0).data(), l, false, maxR)); // behind

    CHECK(inLobe(obs, kIdentityQuat, atAzEl(0, 29.9).data(), l, false, maxR));
    CHECK(inLobe(obs, kIdentityQuat, atAzEl(0, 30.0).data(), l, false, maxR)); // ON the boundary
    CHECK_FALSE(inLobe(obs, kIdentityQuat, atAzEl(0, 30.5).data(), l, false, maxR));
    CHECK_FALSE(inLobe(obs, kIdentityQuat, atAzEl(0, -45).data(), l, false, maxR));
}

TEST_CASE("inLobe: the cone follows the observer's attitude", "[sensor]") {
    const SensorLobe l = lobe(30.f, 30.f, 0.f, 40.f, 1.f);
    const double obs[3] = {0, 0, 0};
    const float maxR = effectiveMaxRangeM(l, SensorType::Radar, kBaseline);

    // A target off to the RIGHT (+Z): outside a nose-forward 30° cone...
    const double right[3] = {0.0, 0.0, 10.0 * kMPerNm};
    CHECK_FALSE(inLobe(obs, kIdentityQuat, right, l, false, maxR));

    // ...but dead ahead once the observer yaws right (Ry(-90°) maps +X to +Z).
    CHECK(inLobe(obs, yawQuat(-90.f).data(), right, l, false, maxR));
}

TEST_CASE("inLobe: an omnidirectional sensor has no cone but still has a range band", "[sensor]") {
    SensorLobe l = lobe(180.f, 90.f, 0.f, 40.f, 1.f);
    const double obs[3] = {0, 0, 0};
    const float maxR = effectiveMaxRangeM(l, SensorType::Radar, kBaseline);

    const double behind[3] = {-10.0 * kMPerNm, 0.0, 0.0};
    CHECK(inLobe(obs, kIdentityQuat, behind, l, /*omnidirectional=*/true, maxR));
    CHECK_FALSE(inLobe(obs, kIdentityQuat, ahead(50.0 * kMPerNm).data(), l, true, maxR)); // range still applies
}

// ── probability ──────────────────────────────────────────────────────────────

TEST_CASE("effectivePod: the default skill of 0.5 is exactly unity", "[sensor]") {
    const SensingEnvironment env;

    // An entity with no [ai] section detects at precisely the probability the sensor def states.
    CHECK_THAT(effectivePod(0.35f, 0.5f, SensorType::Radar, env), WithinRel(0.35f, 1e-6f));

    CHECK_THAT(effectivePod(0.35f, 1.0f, SensorType::Radar, env), WithinRel(0.525f, 1e-6f)); // ace
    CHECK_THAT(effectivePod(0.35f, 0.0f, SensorType::Radar, env), WithinRel(0.175f, 1e-6f)); // rookie

    // Clamped to a probability.
    CHECK_THAT(effectivePod(0.9f, 1.0f, SensorType::Radar, env), WithinRel(1.0f, 1e-6f));
}

TEST_CASE("detectionHash is deterministic and independent of evaluation order", "[sensor]") {
    // The determinism guarantee: same inputs, same value — always, on any worker, on any platform.
    CHECK(detectionHash(3, 7, 100, 0, 0) == detectionHash(3, 7, 100, 0, 0));

    // And it separates every input it is given.
    CHECK(detectionHash(3, 7, 100, 0, 0) != detectionHash(4, 7, 100, 0, 0)); // observer
    CHECK(detectionHash(3, 7, 100, 0, 0) != detectionHash(3, 8, 100, 0, 0)); // target
    CHECK(detectionHash(3, 7, 100, 0, 0) != detectionHash(3, 7, 101, 0, 0)); // tick
    CHECK(detectionHash(3, 7, 100, 0, 0) != detectionHash(3, 7, 100, 1, 0)); // sensor slot
    CHECK(detectionHash(3, 7, 100, 0, 0) != detectionHash(3, 7, 100, 0, 1)); // search vs track lobe
}

TEST_CASE("rollPasses: certainty and impossibility are exact, and the middle is roughly fair", "[sensor]") {
    CHECK(rollPasses(0xFFFFFFFFu, 1.0f));       // pod 1: always
    CHECK(rollPasses(0u, 1.0f));                //
    CHECK_FALSE(rollPasses(0u, 0.0f));          // pod 0: never
    CHECK_FALSE(rollPasses(0xFFFFFFFFu, 0.0f)); //

    // Frequency check over a wide sweep of seeds: a 0.25 PoD should fire about a quarter of the
    // time. This is what makes the integer compare a probability and not just a hash.
    int hits = 0;
    constexpr int kTrials = 4000;
    for (int i = 0; i < kTrials; ++i) {
        const uint32_t h = detectionHash(1u, static_cast<uint32_t>(i), 42u, 0u, 0u);
        if (rollPasses(h, 0.25f))
            ++hits;
    }
    const double rate = static_cast<double>(hits) / kTrials;
    CHECK(rate > 0.22);
    CHECK(rate < 0.28);
}

// ── one sensor against one target ────────────────────────────────────────────

TEST_CASE("evaluateSensor: a target ahead and close is in both lobes", "[sensor]") {
    const SensorDef s = radar();
    const double obs[3] = {0, 0, 0};
    const SensingEnvironment env;

    const SensorEvaluation ev = evaluateSensor(s, /*emitting=*/true, obs, kIdentityQuat, ahead(20.0 * kMPerNm).data(),
                                               kBaseline, 0.5f, env, 1.f, 1, 2, 60, 0);
    CHECK(ev.searchInLobe);
    CHECK(ev.trackInLobe);
    CHECK(ev.searchRollPass); // pod = 1 in the fixture
    CHECK(ev.trackRollPass);
}

TEST_CASE("evaluateSensor: emitting = false blinds a radar entirely, search and track alike", "[sensor]") {
    // The EMCON gate (#526): a radar sees nothing it does not first illuminate. Switch the
    // transmitter off and BOTH lobes go dark — there is no passive-radar search free lunch.
    const SensorDef s = radar();
    const double obs[3] = {0, 0, 0};
    const SensingEnvironment env;

    const SensorEvaluation ev = evaluateSensor(s, /*emitting=*/false, obs, kIdentityQuat, ahead(20.0 * kMPerNm).data(),
                                               kBaseline, 0.5f, env, 1.f, 1, 2, 60, 0);
    CHECK_FALSE(ev.searchInLobe);
    CHECK_FALSE(ev.searchRollPass);
    CHECK_FALSE(ev.trackInLobe);
    CHECK_FALSE(ev.trackRollPass);
}

TEST_CASE("evaluateSensor: allowTrack = false suppresses the lock (radar Search mode)", "[sensor]") {
    // Radar Search mode (#526): report a bearing without ever offering a firing solution. The search
    // lobe works; the track lobe is held off regardless of geometry.
    const SensorDef s = radar();
    const double obs[3] = {0, 0, 0};
    const SensingEnvironment env;

    const SensorEvaluation ev = evaluateSensor(s, /*emitting=*/true, obs, kIdentityQuat, ahead(20.0 * kMPerNm).data(),
                                               kBaseline, 0.5f, env, 1.f, 1, 2, 60, 0, /*allowTrack=*/false);
    CHECK(ev.searchInLobe);
    CHECK(ev.searchRollPass);
    CHECK_FALSE(ev.trackInLobe); // no lock in Search
}

TEST_CASE("evaluateSensor: a passive sensor ignores the emitting flag", "[sensor]") {
    SensorDef irst = radar(/*emitter=*/false);
    irst.type = SensorType::Ir;

    const double obs[3] = {0, 0, 0};
    const SensingEnvironment env;

    const SensorEvaluation ev = evaluateSensor(irst, /*emitting=*/false, obs, kIdentityQuat,
                                               ahead(20.0 * kMPerNm).data(), kBaseline, 0.5f, env, 1.f, 1, 2, 60, 0);
    CHECK(ev.searchInLobe);
    CHECK(ev.trackInLobe); // an IRST does not need to radiate to stare at you
}

TEST_CASE("evaluateSensor: the builtin eyeball has no track lobe to enter", "[sensor]") {
    const SensorDef& eye = BuiltinSensors::eyeball();
    const double obs[3] = {0, 0, 0};
    const SensingEnvironment env;

    const SensorEvaluation ev =
        evaluateSensor(eye, true, obs, kIdentityQuat, ahead(2000.0).data(), kBaseline, 1.0f, env, 1.f, 1, 2, 60, 0);
    CHECK(ev.searchInLobe);
    CHECK_FALSE(ev.trackInLobe); // search-only: an eyeball finds you, it does not lock you
}

// ── the contact state machine ────────────────────────────────────────────────

namespace {

SensorEvaluation ev(bool searchIn, bool trackIn, bool searchRoll = true, bool trackRoll = true) {
    return SensorEvaluation{searchIn, trackIn, searchRoll, trackRoll};
}

} // namespace

TEST_CASE("stepContact: acquisition needs the die, retention does not", "[sensor]") {
    // THE CENTRAL RULE. A 0.35-PoD radar must not drop and re-acquire an untouched target several
    // times a second just because the die came up short.
    ContactTrack c;

    // In the cone, but the roll fails: still Lost.
    c = stepContact(c, ev(true, false, /*searchRoll=*/false), 4.f, 0.1f, 10);
    CHECK(c.state == ContactState::Lost);

    // The roll passes: Detected, and the acquisition tick is stamped (the reaction delay hangs off
    // it in #685).
    c = stepContact(c, ev(true, false, /*searchRoll=*/true), 4.f, 0.1f, 11);
    CHECK(c.state == ContactState::Detected);
    CHECK(c.firstDetectedTick == 11);

    // Now the die fails on every subsequent check, and the contact is STILL held — because it is
    // held by geometry, not by luck.
    for (uint64_t t = 12; t < 20; ++t) {
        c = stepContact(c, ev(true, false, /*searchRoll=*/false), 4.f, 0.1f, t);
        REQUIRE(c.state == ContactState::Detected);
    }
    CHECK(c.lastSeenTick == 19);
    CHECK(c.firstDetectedTick == 11); // unchanged by retention
}

TEST_CASE("stepContact: promotion to Locked, and demotion when the track lobe loses it", "[sensor]") {
    ContactTrack c;
    c = stepContact(c, ev(true, false), 4.f, 0.1f, 1);
    REQUIRE(c.state == ContactState::Detected);

    // In the track lobe, track die passes: Locked.
    c = stepContact(c, ev(true, true, true, /*trackRoll=*/true), 4.f, 0.1f, 2);
    CHECK(c.state == ContactState::Locked);

    // Stays locked while it remains in the track lobe, even with failing dice.
    c = stepContact(c, ev(true, true, false, /*trackRoll=*/false), 4.f, 0.1f, 3);
    CHECK(c.state == ContactState::Locked);

    // Out of the track lobe but still in the search lobe: demote to Detected, NOT coast. We can
    // still see it, we just cannot shoot it.
    c = stepContact(c, ev(true, false), 4.f, 0.1f, 4);
    CHECK(c.state == ContactState::Detected);
    CHECK(c.coastRemainingS == 0.f);
}

TEST_CASE("stepContact: a track lobe entered without a passing die does not promote", "[sensor]") {
    ContactTrack c;
    c = stepContact(c, ev(true, false), 4.f, 0.1f, 1);
    REQUIRE(c.state == ContactState::Detected);

    c = stepContact(c, ev(true, true, true, /*trackRoll=*/false), 4.f, 0.1f, 2);
    CHECK(c.state == ContactState::Detected); // in the lobe, but the lock did not take
}

TEST_CASE("stepContact: losing the cone coasts for lock_hold_s, then drops", "[sensor]") {
    ContactTrack c;
    c = stepContact(c, ev(true, true), 4.f, 0.1f, 1);
    REQUIRE(c.state == ContactState::Locked);

    // Gone from the cone: coast begins, seeded with lock_hold_s.
    c = stepContact(c, ev(false, false), 4.f, 0.1f, 2);
    CHECK(c.state == ContactState::Coasting);
    CHECK_THAT(c.coastRemainingS, WithinRel(4.0f, 1e-5f));

    // Coast runs down in WALL seconds, so the cadence does not change how long a lock survives.
    for (int i = 0; i < 39; ++i)
        c = stepContact(c, ev(false, false), 4.f, 0.1f, 3);
    CHECK(c.state == ContactState::Coasting);
    CHECK(c.coastRemainingS > 0.f);

    c = stepContact(c, ev(false, false), 4.f, 0.1f, 43);
    CHECK(c.state == ContactState::Lost);
    CHECK(c.coastRemainingS == 0.f);
    CHECK(c.firstDetectedTick == 0); // cleared, so a re-acquisition stamps a fresh one
}

TEST_CASE("stepContact: re-entering the cone during the coast recovers without a new roll", "[sensor]") {
    // It was never lost, only unobserved — so no die is owed.
    ContactTrack c;
    c = stepContact(c, ev(true, true), 4.f, 0.1f, 1);
    REQUIRE(c.state == ContactState::Locked);

    c = stepContact(c, ev(false, false), 4.f, 1.0f, 2);
    REQUIRE(c.state == ContactState::Coasting);

    c = stepContact(c, ev(true, false, /*searchRoll=*/false), 4.f, 1.0f, 3);
    CHECK(c.state == ContactState::Detected);
    CHECK(c.coastRemainingS == 0.f);
}

TEST_CASE("stepContact: a search-only sensor drops the instant the target leaves its cone", "[sensor]") {
    // lock_hold_s = 0 (the eyeball). You have not "lost track" of something you were only looking
    // at — you have simply stopped seeing it.
    ContactTrack c;
    c = stepContact(c, ev(true, false), /*lockHoldS=*/0.f, 0.1f, 1);
    REQUIRE(c.state == ContactState::Detected);

    c = stepContact(c, ev(false, false), 0.f, 0.1f, 2);
    CHECK(c.state == ContactState::Lost);
}

TEST_CASE("stepContact: an unheld contact outside the cone stays Lost", "[sensor]") {
    ContactTrack c;
    c = stepContact(c, ev(false, false), 4.f, 0.1f, 1);
    CHECK(c.state == ContactState::Lost);
    CHECK(c.coastRemainingS == 0.f);
}

// ── environment modifiers (#209) ─────────────────────────────────────────────

TEST_CASE("environmentPodScale: clear daylight costs exactly nothing", "[sensor]") {
    // LOAD-BEARING. Every authored `pod` is quoted against clear daylight, so fair weather must be
    // the exact identity — not 0.99. Otherwise every content pack's numbers quietly mean something
    // slightly different than they say, and #684's baselines all shift.
    const SensingEnvironment clear;
    for (auto t : {SensorType::Visual, SensorType::Ir, SensorType::Radar, SensorType::Laser})
        CHECK(environmentPodScale(t, clear) == 1.f);

    // And effectivePod is therefore unchanged from the pre-#209 value in fair weather.
    CHECK_THAT(effectivePod(0.35f, 0.5f, SensorType::Visual, clear), WithinRel(0.35f, 1e-6f));
}

TEST_CASE("environmentPodScale: the dark is the visual channel's problem, and only its", "[sensor]") {
    SensingEnvironment night;
    night.isNight = true;
    night.timeOfDayH = 2.f;

    // An unaided eye loses most of its chance at night...
    CHECK(environmentPodScale(SensorType::Visual, night) < 0.5f);

    // ...while a jet engine is exactly as hot at midnight, radar does not care what time it is, and
    // a laser is not looking for daylight either. This is why night attacks work and why an
    // IR-equipped aircraft owns the night.
    CHECK(environmentPodScale(SensorType::Ir, night) == 1.f);
    CHECK(environmentPodScale(SensorType::Radar, night) == 1.f);
    CHECK(environmentPodScale(SensorType::Laser, night) == 1.f);
}

TEST_CASE("environmentPodScale: weather hits the channels in the right order", "[sensor]") {
    SensingEnvironment storm;
    storm.cloudCoverage = 1.f;
    storm.fogDensity = 0.8f;

    const float vis = environmentPodScale(SensorType::Visual, storm);
    const float ir = environmentPodScale(SensorType::Ir, storm);
    const float radar = environmentPodScale(SensorType::Radar, storm);

    // Radar barely notices; IR is hurt by the moisture; the eyeball suffers most. That ordering is
    // the whole reason an aircraft carries more than one kind of sensor.
    CHECK(vis < ir);
    CHECK(ir < radar);
    CHECK(radar > 0.75f); // rain clutter costs it ~20% in a FULL storm; the eyeball loses ~85%
}

TEST_CASE("environmentPodScale: no weather ever makes a target mathematically undetectable", "[sensor]") {
    // A hard zero would be an invisibility cloak issued by the weather. The worst conditions leave a
    // small chance — acquiring just takes a very long time.
    SensingEnvironment worst;
    worst.cloudCoverage = 1.f;
    worst.fogDensity = 1.f;
    worst.isNight = true;

    for (auto t : {SensorType::Visual, SensorType::Ir, SensorType::Radar, SensorType::Laser}) {
        const float s = environmentPodScale(t, worst);
        CHECK(s > 0.f);
        CHECK(s <= 1.f);
    }
    CHECK(effectivePod(0.35f, 0.5f, SensorType::Visual, worst) > 0.f);
}

TEST_CASE("environmentPodScale: degradation is monotonic in cloud cover", "[sensor]") {
    float prev = 1.f;
    for (float c : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
        SensingEnvironment env;
        env.cloudCoverage = c;
        const float s = environmentPodScale(SensorType::Visual, env);
        CHECK(s <= prev); // thicker cloud never helps
        prev = s;
    }
}

// ── IFF classification (#527) ────────────────────────────────────────────────

TEST_CASE("classifyIff: a friend squawks and is known at any range", "[sensor][iff]") {
    // A friendly relationship is Friend regardless of how it is held — no VID needed. This is the
    // safe direction: the engine never mislabels a friend.
    CHECK(classifyIff(FactionRelation::Friendly, /*mask=*/0u, /*firing=*/false) == Identification::Friend);
    const uint8_t radarOnly = 1u << static_cast<int>(SensorType::Radar);
    CHECK(classifyIff(FactionRelation::Friendly, radarOnly, false) == Identification::Friend);
}

TEST_CASE("classifyIff: a neutral is Unknown", "[sensor][iff]") {
    const uint8_t visual = 1u << static_cast<int>(SensorType::Visual);
    CHECK(classifyIff(FactionRelation::Neutral, visual, true) == Identification::Unknown);
}

TEST_CASE("classifyIff: a hostile is Unknown until positively identified", "[sensor][iff]") {
    const uint8_t radar = 1u << static_cast<int>(SensorType::Radar);
    const uint8_t visual = 1u << static_cast<int>(SensorType::Visual);

    // A bare radar blip on a hostile is NOT a foe — it is unknown. This is the ROE-critical case: a
    // careless BVR shot at an unknown can be a friendly-fire kill.
    CHECK(classifyIff(FactionRelation::Hostile, radar, /*firing=*/false) == Identification::Unknown);

    // Eyes on it (VID) makes it a foe...
    CHECK(classifyIff(FactionRelation::Hostile, visual, false) == Identification::Foe);
    // ...as does a committed firing-quality (STT) lock.
    CHECK(classifyIff(FactionRelation::Hostile, radar, /*firing=*/true) == Identification::Foe);
}

TEST_CASE("affiliationRelation: faction 0 is neutral, distinct non-zero are hostile", "[sensor][iff]") {
    CHECK(affiliationRelation(0, 5) == FactionRelation::Neutral);
    CHECK(affiliationRelation(5, 0) == FactionRelation::Neutral);
    CHECK(affiliationRelation(3, 3) == FactionRelation::Friendly);
    CHECK(affiliationRelation(1, 2) == FactionRelation::Hostile);
}
