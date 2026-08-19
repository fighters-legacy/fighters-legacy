// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fly any IEntityController through the real FlightIntegrator and report what the aircraft did
// (#1143). Extracted from the #1141 loiter work, where closing this loop was the only thing that
// found the defect: every one of the three bugs there — an unbounded roll, a 30 deg sideslip, an
// airspeed with no controller — is invisible to a single-sample assertion on ControlInput signs,
// which is all the AI controllers had.
//
// The closed loop is assembled exactly as WorldBroadcaster::stepFlightSim does it: sample() sees an
// EntityState carrying position, orientation AND world velocity, and its ControlInput drives the
// next integrator step.

#include "ai/Guidance.h" // sideslipOf — this header measures it, so it owns the include
#include "entity/EntityState.h"
#include "entity/IEntityController.h"
#include "flight/BuiltinFlightModel.h"
#include "flight/FlightIntegrator.h"
#include "flight/Geodetic.h"
#include "flight/LocalFrame.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace fl::test {

inline constexpr double kHarnessR = fl::kEarthRadiusM;
inline constexpr float kHarnessDt = 1.f / 60.f;
inline constexpr float kRadToDeg = 57.29577951f;

// What an aircraft did over a run. The bank and sideslip extremes are the #1141/#1143 signals: an
// orbit that ends up inverted, or a turn flown half sideways, both show up here and nowhere else.
struct FlightTrace {
    double startAltM{0.0};
    double minAltM{0.0};
    double maxAltM{0.0};
    double endAltM{0.0};
    float maxAbsBankDeg{0.f};
    float maxAbsSideslipDeg{0.f};
    float endSpeedMps{0.f};
    double crashTimeS{-1.0}; // < 0 = never reached the ground
    double secondsFlown{0.0};

    [[nodiscard]] bool crashed() const noexcept {
        return crashTimeS >= 0.0;
    }
    [[nodiscard]] double altitudeSwingM() const noexcept {
        return maxAltM - minAltM;
    }
};

inline FlightState levelStateAt(double x, double altM, double z, float speedMps, float headingRad = 0.f) {
    FlightState s;
    s.pos_world[0] = x;
    s.pos_world[1] = altM;
    s.pos_world[2] = z;
    s.vel_body[0] = speedMps;
    // Yaw about the local up (+Y near the origin): identity faces +X.
    const glm::quat q = glm::angleAxis(-headingRad, glm::vec3(0.f, 1.f, 0.f));
    s.quat[0] = q.x;
    s.quat[1] = q.y;
    s.quat[2] = q.z;
    s.quat[3] = q.w;
    s.throttle_actual = 0.5f;
    return s;
}

// Level flight at a real latitude/longitude, facing north. The anchored-mission counterpart of
// levelStateAt: away from the origin world +Y is NOT up, so the orientation is built from the local
// ENU basis (body +X = north, +Y = local up, +Z = east). The closed-loop tests historically ran only
// near the origin, where pos.y happens to equal altitude — which is exactly how a controller reading
// pos.y as altitude (#1221) stayed green while flying itself into the terrain at the sandbox home.
inline FlightState levelStateAtGeo(double latRad, double lonRad, double altM, float speedMps) {
    double x = 0.0, y = 0.0, z = 0.0;
    fl::geodeticToWorld(fl::LatLonAlt{latRad, lonRad, altM}, x, y, z, kHarnessR);
    const glm::mat3 enu = fl::enuBasis(glm::dvec3(x, y, z), kHarnessR);
    const glm::quat q = glm::quat_cast(glm::mat3(enu[1], enu[2], enu[0])); // fwd=north, up=up, right=east
    FlightState s;
    s.pos_world[0] = x;
    s.pos_world[1] = y;
    s.pos_world[2] = z;
    s.vel_body[0] = speedMps;
    s.quat[0] = q.x;
    s.quat[1] = q.y;
    s.quat[2] = q.z;
    s.quat[3] = q.w;
    s.throttle_actual = 0.5f;
    return s;
}

// Build the EntityState a controller sees from the integrator's flight state.
inline fl::EntityState entityStateFrom(const FlightState& fs, fl::EntityId id = {1, 1}) {
    fl::EntityState es{};
    es.id = id;
    for (int k = 0; k < 3; ++k)
        es.transform.pos[k] = fs.pos_world[k];
    for (int k = 0; k < 4; ++k)
        es.transform.quat[k] = fs.quat[k];
    const glm::quat q(fs.quat[3], fs.quat[0], fs.quat[1], fs.quat[2]);
    const glm::vec3 velWorld = q * glm::vec3(static_cast<float>(fs.vel_body[0]), static_cast<float>(fs.vel_body[1]),
                                             static_cast<float>(fs.vel_body[2]));
    es.transform.vel[0] = velWorld.x;
    es.transform.vel[1] = velWorld.y;
    es.transform.vel[2] = velWorld.z;
    es.hp = 100.f;
    es.maxHp = 100.f;
    return es;
}

// Per-tick hook: gets the tick index and the controlled entity's state, so a test can drive a
// target/lead entity (the scenario that makes a heading error PERSIST is usually another aircraft
// that keeps turning).
using TickHook = std::function<void(uint64_t tick, const fl::EntityState& own)>;

// Fly `seconds` of `ctrl`. Stops early on ground contact and records when. `ctxFn` supplies the
// AiTickContext for controllers that need one (sensor contacts etc.); the default is an empty
// context, which is NORMATIVE — "not evaluated here", not "saw nothing". `model` selects the
// airframe; the default is the builtin fighter-agility model, and passing one is how a test flies
// a controller against an airframe class the builtin cannot stand in for (#1186's heavy bomber).
inline FlightTrace flyController(fl::IEntityController& ctrl, FlightState init, int seconds,
                                 const TickHook& tickHook = {}, const std::function<fl::AiTickContext()>& ctxFn = {},
                                 std::shared_ptr<const FlightModelData> model = {}) {
    FlightIntegrator integ(model ? std::move(model) : BuiltinFlightModel::get());
    integ.reset(init);
    PayloadEffect payload{};

    auto altOf = [&]() {
        const auto& p = integ.state().pos_world;
        return fl::localAltitude(glm::dvec3(p[0], p[1], p[2]), kHarnessR);
    };

    FlightTrace t{};
    t.startAltM = altOf();
    t.minAltM = t.startAltM;
    t.maxAltM = t.startAltM;

    const int ticks = 60 * seconds;
    for (int i = 0; i < ticks; ++i) {
        const fl::EntityState es = entityStateFrom(integ.state());
        if (tickHook)
            tickHook(static_cast<uint64_t>(i), es);
        const fl::AiTickContext ctx = ctxFn ? ctxFn() : fl::AiTickContext{};
        const ControlInput ci = ctrl.sample(es, static_cast<uint64_t>(i), kHarnessDt, ctx);
        integ.step(kHarnessDt, ci, payload);

        const auto& p = integ.state().pos_world;
        const glm::dvec3 wp(p[0], p[1], p[2]);
        const double a = altOf();
        t.minAltM = std::min(t.minAltM, a);
        t.maxAltM = std::max(t.maxAltM, a);
        t.maxAbsBankDeg =
            std::max(t.maxAbsBankDeg, std::abs(fl::bankOf(integ.state().quat, wp, kHarnessR)) * kRadToDeg);
        const fl::EntityState after = entityStateFrom(integ.state());
        t.maxAbsSideslipDeg = std::max(
            t.maxAbsSideslipDeg, std::abs(fl::ai::sideslipOf(after.transform.quat, after.transform.vel)) * kRadToDeg);
        t.secondsFlown = (i + 1) / 60.0;
        if (a < 1.0) {
            t.crashTimeS = t.secondsFlown;
            break;
        }
    }
    t.endAltM = altOf();
    const auto& fs = integ.state();
    t.endSpeedMps = static_cast<float>(
        std::sqrt(fs.vel_body[0] * fs.vel_body[0] + fs.vel_body[1] * fs.vel_body[1] + fs.vel_body[2] * fs.vel_body[2]));
    return t;
}

} // namespace fl::test
