// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "flight/Atmosphere.h"
#include "flight/BuiltinFlightModel.h"
#include "flight/EngineFailFlags.h"
#include "flight/FixedWingForceModel.h"
#include "flight/FlightIntegrator.h"
#include "flight/IForceModel.h"
#include "flight/IGravityField.h"

#include <array>
#include <cmath>

using Catch::Matchers::WithinAbs;
using namespace fl;

// ---------------------------------------------------------------------------
// 9c: IForceModel / FixedWingForceModel
// ---------------------------------------------------------------------------

TEST_CASE("FixedWingForceModel produces forward thrust at full throttle", "[force_model]") {
    auto data = BuiltinFlightModel::get();
    AtmosphereState atmos = computeAtmosphere(500.f);

    FlightState s{};
    s.quat[3] = 1.f;          // identity orientation
    s.vel_body[0] = 100.f;    // forward airspeed
    s.throttle_actual = 1.0f; // spooled to full

    ControlInput ctrl{};
    ctrl.throttle = 1.0f;

    AeroInputs aero{};
    aero.speed_m_s = 100.f;
    aero.mach = 100.f / 340.f;

    ForceMoment fm = FixedWingForceModel::instance().compute(s, ctrl, {}, *data, atmos, aero);
    CHECK(fm.force_body[0] > 0.f);           // thrust dominates drag along +X
    CHECK(std::isfinite(fm.moment_body[1])); // moments are well-defined
}

// #308: a single engine out removes 1/engine_count of the thrust (not a hardcoded half) and yaws
// toward the dead engine; the lost thrust and the yaw both scale with the engine count.
TEST_CASE("engine-out asymmetry scales with engine_count", "[force_model]") {
    FlightModelData twin = *BuiltinFlightModel::get();
    twin.engine.engine_count = 2; // the default; explicit for clarity
    FlightModelData quad = twin;
    quad.engine.engine_count = 4;

    AtmosphereState atmos = computeAtmosphere(500.f);
    FlightState base{};
    base.quat[3] = 1.f;
    base.vel_body[0] = 200.f;
    base.throttle_actual = 1.0f;
    ControlInput ctrl{};
    ctrl.throttle = 1.0f;
    AeroInputs aero{};
    aero.speed_m_s = 200.f;
    aero.mach = 200.f / 340.f;

    const auto& model = FixedWingForceModel::instance();
    const ForceMoment ok = model.compute(base, ctrl, {}, twin, atmos, aero);

    FlightState fail = base;
    fail.engineFailFlags = kEngineFailLeft; // one engine out
    const ForceMoment ft = model.compute(fail, ctrl, {}, twin, atmos, aero);
    const ForceMoment fq = model.compute(fail, ctrl, {}, quad, atmos, aero);

    const float lostTwin = ok.force_body[0] - ft.force_body[0];
    const float lostQuad = ok.force_body[0] - fq.force_body[0];
    CHECK(lostTwin > 0.f);                                  // a twin sheds half its thrust
    CHECK(lostQuad > 0.f);                                  // a quad sheds a quarter
    CHECK_THAT(lostQuad, WithinAbs(0.5f * lostTwin, 0.5f)); // 1/4 is exactly half of 1/2

    // The yaw moment toward the dead engine scales with the lost thrust (same arm fraction).
    // Engine-out yaw is emitted on the aero yaw axis, moment_body[2].
    const float yawTwin = ft.moment_body[2] - ok.moment_body[2];
    const float yawQuad = fq.moment_body[2] - ok.moment_body[2];
    CHECK(std::abs(yawTwin) > 0.f);
    CHECK_THAT(yawQuad, WithinAbs(0.5f * yawTwin, 1.0f));
}

// #308: a wider engine_yaw_arm_frac produces a larger engine-out yaw for the same lost thrust.
TEST_CASE("engine_yaw_arm_frac scales the engine-out yaw moment", "[force_model]") {
    FlightModelData narrow = *BuiltinFlightModel::get();
    narrow.engine.engine_yaw_arm_frac = 0.15f;
    FlightModelData wide = narrow;
    wide.engine.engine_yaw_arm_frac = 0.30f;

    AtmosphereState atmos = computeAtmosphere(500.f);
    FlightState s{};
    s.quat[3] = 1.f;
    s.vel_body[0] = 200.f;
    s.throttle_actual = 1.0f;
    s.engineFailFlags = kEngineFailRight;
    ControlInput ctrl{};
    ctrl.throttle = 1.0f;
    AeroInputs aero{};
    aero.speed_m_s = 200.f;
    aero.mach = 200.f / 340.f;

    const auto& model = FixedWingForceModel::instance();
    const float yawNarrow = model.compute(s, ctrl, {}, narrow, atmos, aero).moment_body[2];
    const float yawWide = model.compute(s, ctrl, {}, wide, atmos, aero).moment_body[2];
    // Doubling the arm doubles the yaw contribution (same thrust removed).
    CHECK(std::abs(yawWide) > std::abs(yawNarrow));
}

// ---------------------------------------------------------------------------
// Integrator dispatches to the injected seams (not dead abstractions)
// ---------------------------------------------------------------------------

namespace {
struct ZeroGravity final : IGravityField {
    std::array<float, 3> accelWorld(const double[3]) const override {
        return {0.f, 0.f, 0.f};
    }
};

// Strong constant upward body force, zero moment — overwhelms gravity so the craft rises at rest.
struct ConstantUpForce final : IForceModel {
    ForceMoment compute(const FlightState&, const ControlInput&, const PayloadEffect&, const FlightModelData&,
                        const AtmosphereState&, const AeroInputs&) const override {
        return {{0.f, 5.0e5f, 0.f}, {0.f, 0.f, 0.f}};
    }
};

FlightState restingState() {
    FlightState fs{};
    fs.quat[3] = 1.f;          // identity orientation (body == world)
    fs.pos_world[1] = 10000.f; // high enough to never touch the ground floor during the test
    fs.fuel_kg = BuiltinFlightModel::get()->geometry.fuel_kg;
    fs.mass_kg = BuiltinFlightModel::get()->geometry.mass_kg + fs.fuel_kg;
    return fs;
}

double fallTo(FlightIntegrator& fi) {
    ControlInput ctrl{}; // zero throttle: only gravity + the force model act at rest
    for (int i = 0; i < 60; ++i)
        fi.step(1.f / 60.f, ctrl, {}, {}, -1.0e6f);
    return fi.state().pos_world[1];
}
} // namespace

TEST_CASE("FlightIntegrator dispatches to the injected gravity field", "[force_model]") {
    // Default flat gravity: a craft at rest with no thrust falls.
    FlightIntegrator def(BuiltinFlightModel::get());
    def.reset(restingState());
    const double defaultY = fallTo(def);
    CHECK(defaultY < 10000.f); // fell under gravity

    // Zero-g field injected: the same craft stays put.
    FlightIntegrator zero(BuiltinFlightModel::get());
    ZeroGravity zg;
    zero.setGravityField(zg);
    zero.reset(restingState());
    const double zeroY = fallTo(zero);
    CHECK_THAT(zeroY, WithinAbs(10000.0, 1e-2)); // no vertical motion without gravity
    CHECK(zeroY > defaultY);                     // demonstrably different from the default
}

TEST_CASE("FlightIntegrator dispatches to the injected force model", "[force_model]") {
    FlightIntegrator fi(BuiltinFlightModel::get());
    ConstantUpForce up;
    fi.setForceModel(up);
    fi.reset(restingState());
    const double y = fallTo(fi);
    CHECK(y > 10000.0); // the constant upward force model lifted the craft instead of letting it fall
}
