// SPDX-License-Identifier: GPL-3.0-or-later
//
// The builtin coverage contract (#1335): every supported entity-type vocabulary slot has a
// compiled-in builtin — a def, a flight model, or a documented exception — so the next vocabulary
// addition fails a test instead of a release audit. Plus flight sanity for the compiled-in
// rotorcraft models: the #1334 doctrine (realistic but low-performing, never a UFO) applies to
// every model this file guards.

#include "content/ContentBootstrap.h"
#include "entity/EntityTypeRegistry.h"
#include "entity/ObjectCategory.h"
#include "flight/BuiltinFlightModel.h"
#include "flight/FlightIntegrator.h"
#include "flight/ForceModelSelect.h"
#include "weapon/WeaponRegistry.h"

#include "NullLogger.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace fl;

TEST_CASE("builtinFlightModel answers the whole builtin: namespace it owns (#1335)", "[builtin_models]") {
    // One authority for both the server and client resolvers — the pre-#1335 server-only literal
    // left a piloted builtin:carrier predicting on the wrong model.
    const auto trainer = builtinFlightModel("builtin:trainer");
    REQUIRE(trainer);
    CHECK(trainer->isFixedWing());
    CHECK(trainer.get() == BuiltinFlightModel::get().get());

    const auto vessel = builtinFlightModel("builtin:carrier-vessel");
    REQUIRE(vessel);
    CHECK(vessel->isVessel());

    const auto helo = builtinFlightModel("builtin:helicopter");
    REQUIRE(helo);
    CHECK(helo->isHelicopter());
    CHECK(helo->helicopter.has_value());

    const auto quad = builtinFlightModel("builtin:multirotor");
    REQUIRE(quad);
    CHECK(quad->isMultirotor());
    CHECK(quad->multirotor.has_value());

    // An unknown builtin name is NOT a pack lookup — the caller gets null and must error.
    CHECK(builtinFlightModel("builtin:no-such-model") == nullptr);
    CHECK(builtinFlightModel("fl-base:f5e") == nullptr);
}

TEST_CASE("every ObjectCategory has a registered builtin def or a documented exception (#1335)", "[builtin_models]") {
    // Register exactly what fl-server registers zero-pack (ServerRuntime's bootstrap set).
    EntityTypeRegistry registry;
    registry.registerType(builtinDebugEntityDef());
    registry.registerType(builtinBomberDef());
    registry.registerType(builtinSensorFighterDef());
    registry.registerType(builtinParachuteDef());
    registry.registerType(builtinHelicopterDef());
    registry.registerType(builtinMultirotorDef());
    registerBuiltinSurfaceEntities(registry);
    WeaponRegistry weapons;
    registerBuiltinWeapons(weapons);
    NullLogger log;
    registerProjectileEntityDefs(weapons, registry, log);

    bool covered[static_cast<std::size_t>(ObjectCategory::Structure) + 1] = {};
    for (uint32_t i = 0; i < registry.typeCount(); ++i)
        if (const EntityDef* def = registry.byIndex(i))
            covered[static_cast<std::size_t>(def->category)] = true;

    CHECK(covered[static_cast<std::size_t>(ObjectCategory::AirVehicle)]);
    CHECK(covered[static_cast<std::size_t>(ObjectCategory::GroundVehicle)]);
    CHECK(covered[static_cast<std::size_t>(ObjectCategory::NavalVehicle)]);
    CHECK(covered[static_cast<std::size_t>(ObjectCategory::Projectile)]);
    CHECK(covered[static_cast<std::size_t>(ObjectCategory::Effect)]);
    CHECK(covered[static_cast<std::size_t>(ObjectCategory::Structure)]);
    // DOCUMENTED EXCEPTION — ObjectCategory::Player is RESERVED (#1335): pack-parseable, used by no
    // compiled-in def, and player pilots fly AirVehicle defs (the [world] player_entity_type path).
    // If a def starts claiming it, this pin is the prompt to decide what the category now means.
    CHECK_FALSE(covered[static_cast<std::size_t>(ObjectCategory::Player)]);
}

TEST_CASE("every force-model kind has a compiled-in builtin or a documented exception (#1335)", "[builtin_models]") {
    CHECK(builtinFlightModel("builtin:trainer")->isFixedWing());
    CHECK(builtinFlightModel("builtin:carrier-vessel")->isVessel());
    CHECK(builtinFlightModel("builtin:helicopter")->isHelicopter());
    CHECK(builtinFlightModel("builtin:multirotor")->isMultirotor());
    // DOCUMENTED EXCEPTION — no builtin BALLISTIC flyable model (#1335): the builtin weapons'
    // projectiles fly the ProjectileSystem path (`projectile:builtin:*` defs), which is the
    // intended zero-pack ballistic coverage; a pack `type = "ballistic"` model reaches
    // BallisticForceModel when content needs a flyable rocket. Delete this comment and add the
    // model deliberately if that changes.
}

namespace {

// Hover a rotorcraft model closed-loop at a constant collective and report the altitude excursion.
double hoverExcursionM(std::shared_ptr<const FlightModelData> model, float collective, int seconds) {
    FlightIntegrator fi(model);
    applyForceModelFor(fi, *model);
    FlightState s{};
    s.pos_world[1] = 300.0;
    s.mass_kg = model->geometry.mass_kg + model->geometry.fuel_kg;
    s.fuel_kg = model->geometry.fuel_kg;
    s.throttle_actual = collective;
    fi.reset(s);
    ControlInput ctrl{};
    ctrl.throttle = collective;
    PayloadEffect payload{};
    double maxDev = 0.0;
    for (int i = 0; i < 60 * seconds; ++i) {
        fi.step(1.f / 60.f, ctrl, payload);
        maxDev = std::max(maxDev, std::abs(fi.state().pos_world[1] - 300.0));
    }
    return maxDev;
}

} // namespace

TEST_CASE("the builtin helicopter hovers at a plausible collective (#1335)", "[builtin_models]") {
    // Gross 3,600 kg on a 48 kN disc: hover collective ~0.74 — an honest margin, nothing
    // acrobatic. The open-loop hover drifts (flapback, torque), but the vertical axis must be a
    // hover, not a UFO climb or a brick.
    const auto model = builtinFlightModel("builtin:helicopter");
    const float hover = (3600.f * 9.81f) / model->helicopter->main_rotor_max_thrust_n;
    CHECK(hover > 0.6f);
    CHECK(hover < 0.85f);
    CHECK(hoverExcursionM(model, hover, 10) < 60.0);
}

TEST_CASE("the builtin multirotor hovers at a plausible throttle (#1335)", "[builtin_models]") {
    const auto model = builtinFlightModel("builtin:multirotor");
    const float hover = (12.f * 9.81f) / (4.f * model->multirotor->rotor_thrust_max_n);
    CHECK(hover > 0.4f);
    CHECK(hover < 0.7f);
    CHECK(hoverExcursionM(model, hover, 10) < 30.0);
}
