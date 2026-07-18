// SPDX-License-Identifier: GPL-3.0-or-later
#include "sensor/Detection.h"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <numbers>

namespace fl::sensor {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kRadToDeg = 180.f / kPi;

// The acquisition die compares against a 24-bit threshold. Integer, so the determinism guarantee
// does not depend on float ordering across compilers.
constexpr uint32_t kRollDomain = 1u << 24;

// The coast is run down by repeated subtraction, which leaves float dust: 4.0 s decremented forty
// times by 0.1 s lands on ~1.5e-6, not on 0. Without this epsilon a fully expired coast survives one
// extra check — and at a 10 Hz cadence that is a contact held 100 ms past its lock_hold_s, every
// time. Sub-millisecond precision on a coast timer is meaningless anyway.
constexpr float kCoastEpsilonS = 1e-4f;

// What the dark costs an unaided eye (#209). Large on purpose: night attacks work precisely because
// the visual channel mostly stops working, and an IR-equipped aircraft owns the night. A timid value
// here would make darkness cosmetic.
constexpr float kNightVisualScale = 0.25f;

// No channel ever reaches exactly zero — see environmentPodScale.
constexpr float kMinEnvScale = 0.05f;

// Body frame: forward = +X, up = +Y, right = +Z. (+X × +Y = +Z in the engine's Y-up right-handed
// world, which is why +Z is RIGHT and not left — the same convention every AI controller uses.)
struct BodyBasis {
    glm::vec3 forward;
    glm::vec3 up;
    glm::vec3 right;
};

[[nodiscard]] BodyBasis bodyBasis(const float quat[4]) noexcept {
    // EntityTransform stores [x,y,z,w]; the GLM constructor takes (w,x,y,z).
    const glm::quat q(quat[3], quat[0], quat[1], quat[2]);
    return {q * glm::vec3(1.f, 0.f, 0.f), q * glm::vec3(0.f, 1.f, 0.f), q * glm::vec3(0.f, 0.f, 1.f)};
}

} // namespace

float signatureMultiplier(SensorType type, const SignatureDef& sig) noexcept {
    switch (type) {
    case SensorType::Radar:
        return sig.rcs;
    case SensorType::Ir:
        return sig.ir;
    case SensorType::Visual:
        return sig.visual;
    case SensorType::Laser:
        return sig.laser;
    }
    return 1.f;
}

float effectiveMaxRangeM(const SensorLobe& lobe, SensorType type, const SignatureDef& sig,
                         float radarRangeFraction) noexcept {
    const float s = std::max(0.f, signatureMultiplier(type, sig));

    if (type == SensorType::Radar) {
        // sqrt for radar, and the difficulty knob applies to radar only.
        const float fraction = std::clamp(radarRangeFraction, 0.f, 1.f);
        return lobe.maxRangeM * std::sqrt(s) * fraction;
    }
    return lobe.maxRangeM * s;
}

bool inLobe(const double observerPos[3], const float observerQuat[4], const double targetPos[3], const SensorLobe& lobe,
            bool omnidirectional, float effMaxRangeM) noexcept {
    // Range gate first — it is the cheap test, and it rejects most candidates.
    const glm::dvec3 obs(observerPos[0], observerPos[1], observerPos[2]);
    const glm::dvec3 tgt(targetPos[0], targetPos[1], targetPos[2]);
    const glm::dvec3 d = tgt - obs;
    const double dist2 = glm::dot(d, d);

    const double maxR = static_cast<double>(effMaxRangeM);
    const double minR = static_cast<double>(lobe.minRangeM);
    if (dist2 > maxR * maxR)
        return false;
    if (dist2 < minR * minR) // the dead zone: too close to see
        return false;

    // Co-located: no bearing exists, so there is no cone to be inside of. Only an omnidirectional
    // sensor can hold a target it is standing on.
    if (dist2 < 1e-6)
        return omnidirectional;

    if (omnidirectional)
        return true;

    // Cone test in the observer's body frame.
    const glm::vec3 dir = glm::normalize(glm::vec3(d));
    const BodyBasis b = bodyBasis(observerQuat);

    const float fx = glm::dot(dir, b.forward);
    const float fy = glm::dot(dir, b.up);
    const float fz = glm::dot(dir, b.right);

    // Azimuth: angle off the nose in the body's horizontal plane. Elevation: angle above it.
    const float azDeg = std::atan2(fz, fx) * kRadToDeg;
    const float horiz = std::sqrt(fx * fx + fz * fz);
    const float elDeg = std::atan2(fy, horiz) * kRadToDeg;

    // A small epsilon so a target exactly on the boundary is INSIDE. The alternative — a target at
    // precisely the stated half-angle being invisible — would make every authored cone quietly one
    // ULP narrower than its number says.
    constexpr float kEps = 1e-4f;
    return std::abs(azDeg) <= lobe.azHalfAngleDeg + kEps && std::abs(elDeg) <= lobe.elHalfAngleDeg + kEps;
}

float environmentPodScale(SensorType type, const SensingEnvironment& env) noexcept {
    // Clear daylight costs NOTHING — exactly 1.0, not 0.99. Every authored `pod` is quoted against
    // these conditions, so fair weather must be the identity or every pack's numbers quietly drift.
    const float cloud = std::clamp(env.cloudCoverage, 0.f, 1.f);
    const float fog = std::clamp(env.fogDensity, 0.f, 1.f);

    // Weight per channel: how much of this sensor's job the weather takes. Not measured physics —
    // these are behavioural numbers chosen so the four channels stay meaningfully DIFFERENT, which is
    // the point of having four (see the header).
    float cloudWeight = 0.f;
    float fogWeight = 0.f;
    float nightScale = 1.f;

    switch (type) {
    case SensorType::Visual:
        cloudWeight = 0.70f;
        fogWeight = 0.80f;
        nightScale = kNightVisualScale; // the dark is the visual channel's problem, and only its
        break;
    case SensorType::Ir:
        cloudWeight = 0.40f; // moisture attenuates in the infrared...
        fogWeight = 0.50f;
        nightScale = 1.f; // ...but a jet engine is exactly as hot at midnight
        break;
    case SensorType::Radar:
        cloudWeight = 0.15f; // rain clutter, and not much of it
        fogWeight = 0.10f;
        nightScale = 1.f;
        break;
    case SensorType::Laser:
        cloudWeight = 0.70f; // the same line-of-sight problem as the eyeball...
        fogWeight = 0.80f;
        nightScale = 1.f; // ...without the eyeball's dependence on daylight
        break;
    }

    const float scale = (1.f - cloudWeight * cloud) * (1.f - fogWeight * fog) * (env.isNight ? nightScale : 1.f);

    // Never exactly zero: in the worst conditions a sensor still has SOME chance, so a target is
    // never mathematically undetectable — acquiring it just takes a very long time. A hard zero would
    // be an invisibility cloak issued by the weather.
    return std::clamp(scale, kMinEnvScale, 1.f);
}

float effectivePod(float authoredPod, float skill, SensorType type, const SensingEnvironment& env) noexcept {
    // Skill 0.5 (the AiTuning default) is exactly unity, so an entity that authors no [ai] section
    // detects at precisely the probability its sensor def states.
    const float skillScale = 0.5f + std::clamp(skill, 0.f, 1.f);

    return std::clamp(authoredPod * skillScale * environmentPodScale(type, env), 0.f, 1.f);
}

uint32_t detectionHash(uint32_t observerIdx, uint32_t targetIdx, uint64_t tickIndex, uint32_t sensorSlot,
                       uint32_t lobeSalt) noexcept {
    // Same construction as the per-entity turbulence RNG in WorldBroadcaster::stepFlightSim: mix the
    // identifying integers with distinct odd constants, then advance an LCG once. No shared state is
    // mutated, so the value depends on WHO and WHEN, never on evaluation order.
    uint32_t rng =
        observerIdx * 0x9E3779B1u + targetIdx * 0x85EBCA77u + static_cast<uint32_t>(tickIndex) * 0xC2B2AE3Du +
        static_cast<uint32_t>(tickIndex >> 32) * 0x27D4EB2Fu + sensorSlot * 0x165667B1u + lobeSalt * 0x9E3779B9u;
    rng = rng * 1664525u + 1013904223u;
    // A second round: one LCG step leaves the low bits of a small seed poorly mixed, and sensorSlot
    // and lobeSalt are very small integers.
    rng ^= rng >> 15;
    rng = rng * 1664525u + 1013904223u;
    return rng;
}

bool rollPasses(uint32_t hash, float pod) noexcept {
    const float p = std::clamp(pod, 0.f, 1.f);
    if (p <= 0.f)
        return false;
    if (p >= 1.f)
        return true;
    // Integer compare in a 24-bit domain. Take the HIGH bits of the hash: the low bits of an LCG are
    // notoriously weak.
    const auto threshold = static_cast<uint32_t>(p * static_cast<float>(kRollDomain));
    return (hash >> 8) < threshold;
}

SensorEvaluation evaluateSensor(const SensorDef& sensor, bool emitting, const double observerPos[3],
                                const float observerQuat[4], const double targetPos[3], const SignatureDef& targetSig,
                                float skill, const SensingEnvironment& env, float radarRangeFraction,
                                uint32_t observerIdx, uint32_t targetIdx, uint64_t tickIndex, uint32_t sensorSlot,
                                bool allowTrack) noexcept {
    SensorEvaluation ev;

    // A radar or laser detects nothing it does not first illuminate: BOTH lobes require the emitter
    // to be radiating. A passive sensor (IR, visual) receives and ignores the flag. This is the EMCON
    // gate (#526): a radar in Silent mode is blind, not passively omniscient.
    const bool activeType = (sensor.type == SensorType::Radar || sensor.type == SensorType::Laser);
    const bool canReceive = emitting || !activeType;

    if (canReceive) {
        const float searchRange = effectiveMaxRangeM(sensor.search, sensor.type, targetSig, radarRangeFraction);
        ev.searchInLobe =
            inLobe(observerPos, observerQuat, targetPos, sensor.search, sensor.omnidirectional, searchRange);
        if (ev.searchInLobe) {
            const float pod = effectivePod(sensor.search.pod, skill, sensor.type, env);
            ev.searchRollPass = rollPasses(detectionHash(observerIdx, targetIdx, tickIndex, sensorSlot, 0u), pod);
        }
    }

    // The track lobe additionally requires `allowTrack` — false is how radar Search mode reports a
    // bearing without ever offering a firing solution (#526).
    if (sensor.track && canReceive && allowTrack) {
        const float trackRange = effectiveMaxRangeM(*sensor.track, sensor.type, targetSig, radarRangeFraction);
        ev.trackInLobe =
            inLobe(observerPos, observerQuat, targetPos, *sensor.track, sensor.omnidirectional, trackRange);
        if (ev.trackInLobe) {
            const float pod = effectivePod(sensor.track->pod, skill, sensor.type, env);
            ev.trackRollPass = rollPasses(detectionHash(observerIdx, targetIdx, tickIndex, sensorSlot, 1u), pod);
        }
    }

    return ev;
}

ContactTrack stepContact(const ContactTrack& prev, const SensorEvaluation& eval, float lockHoldS, float dtS,
                         uint64_t tickIndex) noexcept {
    ContactTrack next = prev;
    const bool held = prev.state != ContactState::Lost;

    // ── the target is not in the search lobe: coast, then drop ───────────────
    if (!eval.searchInLobe) {
        if (!held)
            return next; // still Lost

        if (prev.state != ContactState::Coasting) {
            // First check since losing it: start the coast.
            next.state = ContactState::Coasting;
            next.coastRemainingS = std::max(0.f, lockHoldS);
        } else {
            next.coastRemainingS = prev.coastRemainingS - std::max(0.f, dtS);
        }

        if (next.coastRemainingS <= kCoastEpsilonS) {
            next.state = ContactState::Lost;
            next.coastRemainingS = 0.f;
            next.firstDetectedTick = 0;
        }
        return next;
    }

    // ── the target IS in the search lobe ─────────────────────────────────────
    // Acquisition needs a die; retention does not (PoD gates acquisition, geometry maintains).
    if (!held) {
        if (!eval.searchRollPass)
            return next; // still Lost — the roll failed, try again next check
        next.state = ContactState::Detected;
        next.firstDetectedTick = tickIndex;
    } else if (prev.state == ContactState::Coasting) {
        // Recovered before the coast ran out: it was never lost, only unobserved. No new roll.
        next.state = ContactState::Detected;
    } else {
        next.state = prev.state; // Detected or Locked, carried forward
    }
    next.coastRemainingS = 0.f;
    next.lastSeenTick = tickIndex;

    // ── promotion to a firing-quality track ──────────────────────────────────
    if (eval.trackInLobe) {
        if (next.state == ContactState::Locked || eval.trackRollPass)
            next.state = ContactState::Locked; // already locked: held by geometry, no new die
    } else if (next.state == ContactState::Locked) {
        // Still visible to the search lobe, but the track lobe has lost it: demote rather than
        // coast — we can still see it, we just cannot shoot it.
        next.state = ContactState::Detected;
    }

    return next;
}

} // namespace fl::sensor
