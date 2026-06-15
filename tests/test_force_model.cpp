// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "flight/Atmosphere.h"
#include "flight/BuiltinFlightModel.h"
#include "flight/FixedWingForceModel.h"
#include "flight/FlightIntegrator.h"
#include "flight/IForceModel.h"
#include "flight/IGravityField.h"

#include <array>
#include <cmath>

using Catch::Matchers::WithinAbs;
using namespace fl;

// ---------------------------------------------------------------------------
// 9d: IGravityField
// ---------------------------------------------------------------------------

TEST_CASE("FlatGravityField returns 9.80665 down regardless of position", "[force_model]") {
    const float a[3] = {123.f, -456.f, 789.f};
    auto g0 = FlatGravityField::instance().accelWorld(a);
    const float b[3] = {0.f, 0.f, 0.f};
    auto g1 = FlatGravityField::instance().accelWorld(b);
    CHECK_THAT(g0[0], WithinAbs(0.0, 1e-6));
    CHECK_THAT(g0[1], WithinAbs(-9.80665, 1e-5));
    CHECK_THAT(g0[2], WithinAbs(0.0, 1e-6));
    CHECK(g1[1] == g0[1]); // position-independent (uniform field)
}

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

// ---------------------------------------------------------------------------
// Integrator dispatches to the injected seams (not dead abstractions)
// ---------------------------------------------------------------------------

namespace {
struct ZeroGravity final : IGravityField {
    std::array<float, 3> accelWorld(const float[3]) const override {
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

float fallTo(FlightIntegrator& fi) {
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
    const float defaultY = fallTo(def);
    CHECK(defaultY < 10000.f); // fell under gravity

    // Zero-g field injected: the same craft stays put.
    FlightIntegrator zero(BuiltinFlightModel::get());
    ZeroGravity zg;
    zero.setGravityField(zg);
    zero.reset(restingState());
    const float zeroY = fallTo(zero);
    CHECK_THAT(zeroY, WithinAbs(10000.0, 1e-2)); // no vertical motion without gravity
    CHECK(zeroY > defaultY);                     // demonstrably different from the default
}

TEST_CASE("FlightIntegrator dispatches to the injected force model", "[force_model]") {
    FlightIntegrator fi(BuiltinFlightModel::get());
    ConstantUpForce up;
    fi.setForceModel(up);
    fi.reset(restingState());
    const float y = fallTo(fi);
    CHECK(y > 10000.f); // the constant upward force model lifted the craft instead of letting it fall
}
