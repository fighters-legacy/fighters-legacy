// SPDX-License-Identifier: GPL-3.0-or-later
#include "ILogger.h"
#include "ai/AiControllerFactory.h"
#include "ai/BreakTurnController.h"
#include "ai/EvadeController.h"
#include "ai/Guidance.h"
#include "ai/LoiterController.h"
#include "ai/PursuitController.h"
#include "ai/StateMachineController.h"
#include "ai/WaypointController.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "spatial/SpatialIndex.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <glm/glm.hpp>
#include <numbers>
#include <string>
#include <vector>

using namespace fl;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

struct NullLogger : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// Build an EntityState at (px,py,pz) with the given quaternion [x,y,z,w].
// Default quaternion is identity (forward = +X world axis).
static fl::EntityState makeState(double px, double py, double pz, float qx = 0.f, float qy = 0.f, float qz = 0.f,
                                 float qw = 1.f) {
    fl::EntityState s{};
    s.id = {1, 1};
    s.transform.pos[0] = px;
    s.transform.pos[1] = py;
    s.transform.pos[2] = pz;
    s.transform.quat[0] = qx;
    s.transform.quat[1] = qy;
    s.transform.quat[2] = qz;
    s.transform.quat[3] = qw;
    return s;
}

// Minimal entity type for EntityManager tests.
static fl::EntityDef makeBasicDef(const char* id = "test:basic") {
    fl::EntityDef d;
    d.id = id;
    d.name = "Basic";
    d.category = fl::ObjectCategory::AirVehicle;
    d.maxHp = 100.f;
    return d;
}

// Quaternion for a pure yaw rotation (around +Y axis) by angleDeg degrees.
// Returns [x,y,z,w] in EntityTransform convention.
static void yawQuat(float angleDeg, float (&q)[4]) {
    float halfRad = angleDeg * 0.5f * static_cast<float>(std::numbers::pi_v<double> / 180.0);
    q[0] = 0.f;
    q[1] = std::sin(halfRad); // y component
    q[2] = 0.f;
    q[3] = std::cos(halfRad); // w component
}

// ---------------------------------------------------------------------------
// Guidance math
// ---------------------------------------------------------------------------

TEST_CASE("Guidance: bodyForward identity quat gives +X world direction") {
    float q[4] = {0.f, 0.f, 0.f, 1.f};
    glm::vec3 fwd = fl::ai::bodyForward(q);
    CHECK(fwd.x == Catch::Approx(1.f).margin(1e-5f));
    CHECK(fwd.y == Catch::Approx(0.f).margin(1e-5f));
    CHECK(fwd.z == Catch::Approx(0.f).margin(1e-5f));
}

TEST_CASE("Guidance: bodyForward 90-degree yaw right gives +Z world direction") {
    // Right yaw = -90 deg around +Y in right-hand Y-up (Ry(-90) maps +X to +Z).
    float q[4] = {};
    yawQuat(-90.f, q);
    glm::vec3 fwd = fl::ai::bodyForward(q);
    CHECK(fwd.x == Catch::Approx(0.f).margin(1e-5f));
    CHECK(fwd.y == Catch::Approx(0.f).margin(1e-5f));
    CHECK(fwd.z == Catch::Approx(1.f).margin(1e-5f));
}

TEST_CASE("Guidance: horizontalHeadingError positive for target to the right") {
    // Entity at origin, forward = +X (identity quat).
    // Right is +Z in Y-up right-hand system with forward=+X.
    float q[4] = {0.f, 0.f, 0.f, 1.f};
    double own[3] = {0.0, 0.0, 0.0};
    double right[3] = {10.0, 0.0, 10.0}; // to the right (+Z side)
    double left[3] = {10.0, 0.0, -10.0}; // to the left (-Z side)

    CHECK(fl::ai::horizontalHeadingError(q, own, right) > 0.f);
    CHECK(fl::ai::horizontalHeadingError(q, own, left) < 0.f);
}

TEST_CASE("Guidance: horizontalHeadingError zero for target directly ahead") {
    float q[4] = {0.f, 0.f, 0.f, 1.f};
    double own[3] = {0.0, 0.0, 0.0};
    double ahead[3] = {1000.0, 0.0, 0.0};
    CHECK(fl::ai::horizontalHeadingError(q, own, ahead) == Catch::Approx(0.f).margin(1e-4f));
}

TEST_CASE("Guidance: bankToTurnAileron clamps to 1.0 for large heading error") {
    // 180 deg error should saturate at 1.0, not exceed it.
    float big = static_cast<float>(std::numbers::pi_v<float>);
    CHECK(fl::ai::bankToTurnAileron(big) == Catch::Approx(1.f).margin(1e-5f));
    CHECK(fl::ai::bankToTurnAileron(-big) == Catch::Approx(-1.f).margin(1e-5f));
}

TEST_CASE("Guidance: pitchErrorFromAlt positive for entity below target altitude") {
    float q[4] = {0.f, 0.f, 0.f, 1.f}; // identity: level flight
    float altErr = 100.f;              // 100 m below target
    float pitchErr = fl::ai::pitchErrorFromAlt(q, altErr);
    CHECK(pitchErr > 0.f); // need to pitch up
}

// ---------------------------------------------------------------------------
// LoiterController
// ---------------------------------------------------------------------------

TEST_CASE("LoiterController: produces right-bank aileron when to the right of center") {
    // Entity at (3000,600,0), center at origin, forward = +X (identity quat).
    // Clockwise orbit: tangent points in +Z → positive aileron.
    fl::EntityState s = makeState(3000.0, 600.0, 0.0);
    fl::ai::LoiterController ctrl({0.0, 600.0, 0.0}, 3000.f, 600.f);
    fl::ControlInput inp = ctrl.sample(s, 0, 1.0 / 60.0);

    CHECK(inp.throttle > 0.f);
    CHECK(inp.aileron > 0.f);
}

TEST_CASE("LoiterController: CounterClockwise direction produces opposite aileron sign") {
    fl::EntityState s = makeState(3000.0, 600.0, 0.0);
    fl::ai::LoiterController cw({0.0, 600.0, 0.0}, 3000.f, 600.f, 0.65f, fl::ai::LoiterDir::Clockwise);
    fl::ai::LoiterController ccw({0.0, 600.0, 0.0}, 3000.f, 600.f, 0.65f, fl::ai::LoiterDir::CounterClockwise);

    fl::ControlInput cwInp = cw.sample(s, 0, 1.0 / 60.0);
    fl::ControlInput ccwInp = ccw.sample(s, 0, 1.0 / 60.0);

    CHECK(cwInp.aileron > 0.f);
    CHECK(ccwInp.aileron < 0.f);
}

TEST_CASE("LoiterController: entity at center returns neutral with throttle set") {
    // Entity exactly at center — should return neutral surfaces (no divide-by-zero).
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    fl::ai::LoiterController ctrl({0.0, 600.0, 0.0});
    fl::ControlInput inp = ctrl.sample(s, 0, 1.0 / 60.0);

    CHECK(inp.throttle > 0.f);
    CHECK(inp.aileron == Catch::Approx(0.f).margin(1e-5f));
    CHECK(inp.elevator == Catch::Approx(0.f).margin(1e-5f));
}

// ---------------------------------------------------------------------------
// WaypointController
// ---------------------------------------------------------------------------

TEST_CASE("WaypointController: aileron positive for waypoint to the right") {
    // Waypoint at (1000,600,1000): to the right (+Z) → positive heading error → positive aileron.
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    fl::ai::WaypointController ctrl({glm::dvec3{1000.0, 600.0, 1000.0}});
    fl::ControlInput inp = ctrl.sample(s, 0, 1.0 / 60.0);

    CHECK(inp.throttle > 0.f);
    CHECK(inp.aileron > 0.f);
}

TEST_CASE("WaypointController: throttle is positive when flying toward waypoint") {
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    fl::ai::WaypointController ctrl({glm::dvec3{5000.0, 600.0, 0.0}});
    fl::ControlInput inp = ctrl.sample(s, 0, 1.0 / 60.0);
    CHECK(inp.throttle > 0.f);
}

TEST_CASE("WaypointController: advances to next waypoint on capture") {
    // wp0 is at entity position — captured immediately; steering uses wp1.
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    fl::ai::WaypointController ctrl({
        glm::dvec3{0.0, 600.0, 0.0},    // wp0: captured immediately
        glm::dvec3{5000.0, 600.0, 0.0}, // wp1: straight ahead
    });
    ctrl.sample(s, 0, 1.0 / 60.0);
    CHECK(ctrl.currentWaypointIndex() == 1);
}

TEST_CASE("WaypointController: loops back when loop=true") {
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    // Single wp at entity position: captured → wraps back to index 0.
    fl::ai::WaypointController ctrl({glm::dvec3{0.0, 600.0, 0.0}}, 500.f, 0.7f, true);
    ctrl.sample(s, 0, 1.0 / 60.0);
    CHECK(ctrl.currentWaypointIndex() == 0);
}

TEST_CASE("WaypointController: returns neutral when all waypoints consumed") {
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    // Single wp at entity position, no loop → consumed after first tick.
    fl::ai::WaypointController ctrl({glm::dvec3{0.0, 600.0, 0.0}}, 500.f, 0.7f, false);
    fl::ControlInput inp = ctrl.sample(s, 0, 1.0 / 60.0);

    CHECK(inp.throttle == Catch::Approx(0.f));
    CHECK(inp.aileron == Catch::Approx(0.f));
    CHECK(inp.elevator == Catch::Approx(0.f));
}

TEST_CASE("WaypointController: empty waypoints returns neutral immediately") {
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    fl::ai::WaypointController ctrl({});
    fl::ControlInput inp = ctrl.sample(s, 0, 1.0 / 60.0);

    CHECK(inp.throttle == Catch::Approx(0.f));
    CHECK(inp.aileron == Catch::Approx(0.f));
}

// ---------------------------------------------------------------------------
// PursuitController
// ---------------------------------------------------------------------------

TEST_CASE("PursuitController: banks toward target entity") {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(makeBasicDef());
    fl::EntityManager em(log, reg);

    fl::EntityTransform ta{};
    ta.quat[3] = 1.f; // identity
    fl::EntityId attackerId = em.spawn("test:basic", ta);

    fl::EntityTransform tt{};
    tt.pos[2] = 2000.0; // 2 km to the right (+Z)
    tt.quat[3] = 1.f;
    fl::EntityId targetId = em.spawn("test:basic", tt);

    fl::ai::PursuitController ctrl(em, targetId);
    const fl::EntityState* as = em.get(attackerId);
    REQUIRE(as != nullptr);
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.throttle > 0.f);
    CHECK(inp.aileron > 0.f); // target is to the right
}

TEST_CASE("PursuitController: afterburner flag propagates") {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(makeBasicDef());
    fl::EntityManager em(log, reg);

    fl::EntityTransform t{};
    t.quat[3] = 1.f;
    fl::EntityId attackerId = em.spawn("test:basic", t);

    fl::EntityTransform tt{};
    tt.pos[2] = 2000.0;
    tt.quat[3] = 1.f;
    fl::EntityId targetId = em.spawn("test:basic", tt);

    fl::ai::PursuitController ctrl(em, targetId, 0.85f, /*useAfterburner=*/true);
    const fl::EntityState* as = em.get(attackerId);
    REQUIRE(as != nullptr);
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.afterburner == true);
}

TEST_CASE("PursuitController: returns neutral when target is dead") {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(makeBasicDef());
    fl::EntityManager em(log, reg);

    fl::EntityTransform t{};
    t.quat[3] = 1.f;
    fl::EntityId attackerId = em.spawn("test:basic", t);
    fl::EntityId targetId = em.spawn("test:basic", t);

    em.kill(targetId);
    em.onTick(1.0 / 60.0, 1);

    fl::ai::PursuitController ctrl(em, targetId);
    const fl::EntityState* as = em.get(attackerId);
    REQUIRE(as != nullptr);
    fl::ControlInput inp = ctrl.sample(*as, 1, 1.0 / 60.0);

    CHECK(inp.throttle == Catch::Approx(0.f));
    CHECK(inp.aileron == Catch::Approx(0.f));
}

TEST_CASE("PursuitController: returns neutral for invalid EntityId") {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(makeBasicDef());
    fl::EntityManager em(log, reg);

    fl::EntityTransform t{};
    t.quat[3] = 1.f;
    fl::EntityId attackerId = em.spawn("test:basic", t);

    fl::ai::PursuitController ctrl(em, fl::EntityId::null());
    const fl::EntityState* as = em.get(attackerId);
    REQUIRE(as != nullptr);
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.throttle == Catch::Approx(0.f));
    CHECK(inp.aileron == Catch::Approx(0.f));
}

// ---------------------------------------------------------------------------
// EvadeController
// ---------------------------------------------------------------------------

TEST_CASE("EvadeController: banks opposite to threat direction") {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(makeBasicDef());
    fl::EntityManager em(log, reg);

    fl::EntityTransform ta{};
    ta.quat[3] = 1.f;
    fl::EntityId attackerId = em.spawn("test:basic", ta);

    fl::EntityTransform tt{};
    tt.pos[2] = 2000.0; // threat to the right (+Z)
    tt.quat[3] = 1.f;
    fl::EntityId threatId = em.spawn("test:basic", tt);

    fl::ai::EvadeController ctrl(em, threatId);
    const fl::EntityState* as = em.get(attackerId);
    REQUIRE(as != nullptr);
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.aileron < 0.f); // threat to right → evade by banking left
}

TEST_CASE("EvadeController: returns neutral when threat is dead") {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(makeBasicDef());
    fl::EntityManager em(log, reg);

    fl::EntityTransform t{};
    t.quat[3] = 1.f;
    fl::EntityId attackerId = em.spawn("test:basic", t);
    fl::EntityId threatId = em.spawn("test:basic", t);

    em.kill(threatId);
    em.onTick(1.0 / 60.0, 1);

    fl::ai::EvadeController ctrl(em, threatId);
    const fl::EntityState* as = em.get(attackerId);
    REQUIRE(as != nullptr);
    fl::ControlInput inp = ctrl.sample(*as, 1, 1.0 / 60.0);

    CHECK(inp.throttle == Catch::Approx(0.f));
    CHECK(inp.aileron == Catch::Approx(0.f));
}

// ---------------------------------------------------------------------------
// BreakTurnController
// ---------------------------------------------------------------------------

TEST_CASE("BreakTurnController: roll phase banks toward threat") {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(makeBasicDef());
    fl::EntityManager em(log, reg);

    fl::EntityTransform ta{};
    ta.quat[3] = 1.f;
    fl::EntityId attackerId = em.spawn("test:basic", ta);

    fl::EntityTransform tt{};
    tt.pos[2] = 2000.0; // threat to the right (+Z)
    tt.quat[3] = 1.f;
    fl::EntityId threatId = em.spawn("test:basic", tt);

    // Use a long roll phase so we stay in Roll on the first tick.
    fl::ai::BreakTurnController ctrl(em, threatId, 10.f);
    const fl::EntityState* as = em.get(attackerId);
    REQUIRE(as != nullptr);
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.aileron > 0.f); // threat is right → roll toward it
    CHECK(inp.elevator == Catch::Approx(0.f).margin(1e-5f));
}

TEST_CASE("BreakTurnController: transitions to pull phase after roll duration") {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(makeBasicDef());
    fl::EntityManager em(log, reg);

    fl::EntityTransform t{};
    t.quat[3] = 1.f;
    em.spawn("test:basic", t); // attacker
    fl::EntityTransform tt{};
    tt.pos[2] = 2000.0;
    tt.quat[3] = 1.f;
    fl::EntityId threatId = em.spawn("test:basic", tt);

    fl::EntityTransform ta{};
    ta.quat[3] = 1.f;
    fl::EntityId attackerId = em.spawn("test:basic", ta);

    fl::ai::BreakTurnController ctrl(em, threatId, 0.5f, 1.f);
    const fl::EntityState* as = em.get(attackerId);
    REQUIRE(as != nullptr);

    // Single call with dt = rollPhaseDuration triggers the transition.
    fl::ControlInput inp = ctrl.sample(*as, 0, 0.5);

    CHECK(inp.elevator == Catch::Approx(1.f).margin(1e-5f));
    CHECK(inp.aileron == Catch::Approx(0.f).margin(1e-5f));
    CHECK(inp.afterburner == true);
}

TEST_CASE("BreakTurnController: accumulated dt triggers phase transition") {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(makeBasicDef());
    fl::EntityManager em(log, reg);

    fl::EntityTransform t{};
    t.quat[3] = 1.f;
    fl::EntityId attackerId = em.spawn("test:basic", t);

    fl::EntityTransform tt{};
    tt.pos[2] = 2000.0;
    tt.quat[3] = 1.f;
    fl::EntityId threatId = em.spawn("test:basic", tt);

    fl::ai::BreakTurnController ctrl(em, threatId, 0.5f, 1.f);
    const fl::EntityState* as = em.get(attackerId);
    REQUIRE(as != nullptr);

    // First half-duration sample: still in Roll phase.
    fl::ControlInput inp1 = ctrl.sample(*as, 0, 0.25);
    CHECK(inp1.aileron != Catch::Approx(0.f).margin(1e-5f));  // Roll: aileron engaged
    CHECK(inp1.elevator == Catch::Approx(0.f).margin(1e-5f)); // not pulling yet

    // Second half-duration sample: timer reaches 0.5 → Pull phase.
    fl::ControlInput inp2 = ctrl.sample(*as, 1, 0.25);
    CHECK(inp2.elevator == Catch::Approx(1.f).margin(1e-5f)); // Pull: full elevator
    CHECK(inp2.aileron == Catch::Approx(0.f).margin(1e-5f));  // aileron released
}

TEST_CASE("BreakTurnController: returns neutral when threat is dead") {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(makeBasicDef());
    fl::EntityManager em(log, reg);

    fl::EntityTransform t{};
    t.quat[3] = 1.f;
    fl::EntityId attackerId = em.spawn("test:basic", t);
    fl::EntityId threatId = em.spawn("test:basic", t);

    em.kill(threatId);
    em.onTick(1.0 / 60.0, 1);

    fl::ai::BreakTurnController ctrl(em, threatId);
    const fl::EntityState* as = em.get(attackerId);
    REQUIRE(as != nullptr);
    fl::ControlInput inp = ctrl.sample(*as, 1, 1.0 / 60.0);

    CHECK(inp.throttle == Catch::Approx(0.f));
    CHECK(inp.aileron == Catch::Approx(0.f));
    CHECK(inp.elevator == Catch::Approx(0.f));
}

// ---------------------------------------------------------------------------
// AiControllerFactory
// ---------------------------------------------------------------------------

TEST_CASE("AiControllerFactory: loiter with no args uses defaults") {
    std::vector<std::string_view> args;
    auto ctrl = fl::ai::createController("loiter", std::span{args});
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: loiter from string args") {
    std::vector<std::string_view> args = {"0", "600", "0"};
    auto ctrl = fl::ai::createController("loiter", std::span{args});
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: loiter with ccw direction") {
    std::vector<std::string_view> args = {"0", "600", "0", "3000", "600", "0.65", "ccw"};
    auto ctrl = fl::ai::createController("loiter", std::span{args});
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: waypoint from string args") {
    std::vector<std::string_view> args = {"1000", "600", "500"};
    auto ctrl = fl::ai::createController("waypoint", std::span{args});
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: waypoint with loop flag") {
    std::vector<std::string_view> args = {"1000", "600", "500", "--loop"};
    auto ctrl = fl::ai::createController("waypoint", std::span{args});
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: waypoint with incomplete triplet returns nullptr") {
    std::vector<std::string_view> args = {"1000", "600"}; // missing z
    auto ctrl = fl::ai::createController("waypoint", std::span{args});
    CHECK(ctrl == nullptr);
}

TEST_CASE("AiControllerFactory: unknown behavior returns nullptr") {
    std::vector<std::string_view> args;
    auto ctrl = fl::ai::createController("unknown", std::span{args});
    CHECK(ctrl == nullptr);
}

// Factory tests that require a live entity in EntityManager.
namespace {
struct FactoryFixture {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    fl::EntityManager em;
    fl::EntityId entityId;

    FactoryFixture() : em(log, reg) {
        reg.registerType(makeBasicDef());
        fl::EntityTransform t{};
        t.quat[3] = 1.f;
        entityId = em.spawn("test:basic", t);
    }
};
} // namespace

TEST_CASE("AiControllerFactory: pursuit from string args") {
    FactoryFixture f;
    std::string idxStr = std::to_string(f.entityId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("pursuit", std::span{args}, &f.em);
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: pursuit with nonexistent entity index returns nullptr") {
    FactoryFixture f;
    std::vector<std::string_view> args = {"9999"};
    auto ctrl = fl::ai::createController("pursuit", std::span{args}, &f.em);
    CHECK(ctrl == nullptr);
}

TEST_CASE("AiControllerFactory: evade from string args") {
    FactoryFixture f;
    std::string idxStr = std::to_string(f.entityId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("evade", std::span{args}, &f.em);
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: break from string args") {
    FactoryFixture f;
    std::string idxStr = std::to_string(f.entityId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("break", std::span{args}, &f.em);
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: break with custom roll duration") {
    FactoryFixture f;
    std::string idxStr = std::to_string(f.entityId.index);
    std::vector<std::string_view> args = {idxStr, "1.0"};
    auto ctrl = fl::ai::createController("break", std::span{args}, &f.em);
    CHECK(ctrl != nullptr);
}

// ---------------------------------------------------------------------------
// StateMachineController — core behavior
// ---------------------------------------------------------------------------

namespace {
struct SmFixture {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    fl::EntityManager em;
    fl::EntityId selfId;
    fl::EntityId targetId;

    SmFixture() : em(log, reg) {
        reg.registerType(makeBasicDef());
        fl::EntityTransform ts{};
        ts.quat[3] = 1.f;
        selfId = em.spawn("test:basic", ts);
        fl::EntityTransform tt{};
        tt.quat[3] = 1.f;
        targetId = em.spawn("test:basic", tt);
    }
};
} // namespace

TEST_CASE("StateMachineController: returns neutral when no initial state set") {
    SmFixture f;
    fl::ai::StateMachineController sm(f.em);
    sm.addState("a", [] { return std::make_unique<fl::ai::LoiterController>(glm::dvec3{0.0, 600.0, 0.0}); });
    // setInitialState NOT called
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    fl::ControlInput inp = sm.sample(s, 0, 1.0 / 60.0);
    CHECK(inp.throttle == Catch::Approx(0.f));
    CHECK(inp.aileron == Catch::Approx(0.f));
    CHECK(inp.elevator == Catch::Approx(0.f));
    CHECK(sm.currentState().empty());
}

TEST_CASE("StateMachineController: delegates to child controller output") {
    SmFixture f;
    fl::ai::StateMachineController sm(f.em);
    sm.addState("loiter",
                [] { return std::make_unique<fl::ai::LoiterController>(glm::dvec3{0.0, 600.0, 0.0}, 3000.f, 600.f); });
    sm.setInitialState("loiter");

    fl::EntityState s = makeState(3000.0, 600.0, 0.0);
    for (int i = 0; i < 5; ++i) {
        fl::ControlInput inp = sm.sample(s, static_cast<uint64_t>(i), 1.0 / 60.0);
        CHECK(inp.throttle > 0.f);
    }
    CHECK(sm.currentState() == "loiter");
}

TEST_CASE("StateMachineController: currentState returns initial state name") {
    SmFixture f;
    fl::ai::StateMachineController sm(f.em);
    sm.addState("patrol", [] { return std::make_unique<fl::ai::LoiterController>(glm::dvec3{}); });
    sm.setInitialState("patrol");
    CHECK(sm.currentState() == "patrol");
}

TEST_CASE("StateMachineController: state with no outgoing transitions stays indefinitely") {
    SmFixture f;
    fl::ai::StateMachineController sm(f.em);
    sm.addState("idle", [] { return std::make_unique<fl::ai::LoiterController>(glm::dvec3{}); });
    sm.setInitialState("idle");

    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    for (int i = 0; i < 100; ++i) {
        sm.sample(s, static_cast<uint64_t>(i), 1.0 / 60.0);
    }
    CHECK(sm.currentState() == "idle");
}

TEST_CASE("StateMachineController: does not transition when condition is false") {
    SmFixture f;
    fl::ai::StateMachineController sm(f.em);
    sm.addState("a", [] { return std::make_unique<fl::ai::LoiterController>(glm::dvec3{}); });
    sm.addState("b", [] { return std::make_unique<fl::ai::LoiterController>(glm::dvec3{}); });
    sm.addTransition("a", "b", fl::ai::Not(fl::ai::Always())); // never true
    sm.setInitialState("a");

    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    for (int i = 0; i < 10; ++i) {
        sm.sample(s, static_cast<uint64_t>(i), 1.0 / 60.0);
    }
    CHECK(sm.currentState() == "a");
}

TEST_CASE("StateMachineController: transitions on first matching condition") {
    SmFixture f;
    fl::ai::StateMachineController sm(f.em);
    sm.addState("a", [] { return std::make_unique<fl::ai::LoiterController>(glm::dvec3{}); });
    sm.addState("b", [] { return std::make_unique<fl::ai::LoiterController>(glm::dvec3{}); });
    sm.addTransition("a", "b", fl::ai::Always());
    sm.setInitialState("a");

    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    sm.sample(s, 0, 1.0 / 60.0); // fires transition at end of tick
    CHECK(sm.currentState() == "b");
}

TEST_CASE("StateMachineController: priority ordering fires first matching transition") {
    // First transition condition is false, second is Always() — second target must win.
    SmFixture f;
    fl::ai::StateMachineController sm(f.em);
    sm.addState("a", [] { return std::make_unique<fl::ai::LoiterController>(glm::dvec3{}); });
    sm.addState("wrong", [] { return std::make_unique<fl::ai::LoiterController>(glm::dvec3{}); });
    sm.addState("right", [] { return std::make_unique<fl::ai::LoiterController>(glm::dvec3{}); });
    sm.addTransition("a", "wrong", fl::ai::Not(fl::ai::Always())); // false — skipped
    sm.addTransition("a", "right", fl::ai::Always());              // true — fires
    sm.setInitialState("a");

    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    sm.sample(s, 0, 1.0 / 60.0);
    CHECK(sm.currentState() == "right");
}

TEST_CASE("StateMachineController: transition to unknown state name is ignored") {
    SmFixture f;
    fl::ai::StateMachineController sm(f.em);
    sm.addState("a", [] { return std::make_unique<fl::ai::LoiterController>(glm::dvec3{}); });
    sm.addTransition("a", "nonexistent", fl::ai::Always());
    sm.setInitialState("a");

    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    sm.sample(s, 0, 1.0 / 60.0);
    CHECK(sm.currentState() == "a"); // no change; unknown target handled gracefully
}

TEST_CASE("StateMachineController: minDwellSeconds blocks premature transition") {
    SmFixture f;
    fl::ai::StateMachineController sm(f.em);
    sm.addState("a", [] { return std::make_unique<fl::ai::LoiterController>(glm::dvec3{}); });
    sm.addState("b", [] { return std::make_unique<fl::ai::LoiterController>(glm::dvec3{}); });
    sm.addTransition("a", "b", fl::ai::Always(), /*minDwellSeconds=*/1.0f);
    sm.setInitialState("a");

    fl::EntityState s = makeState(0.0, 600.0, 0.0);

    // 30 ticks = 0.5 s — should still be in "a"
    for (int i = 0; i < 30; ++i) {
        sm.sample(s, static_cast<uint64_t>(i), 1.0 / 60.0);
    }
    CHECK(sm.currentState() == "a");

    // 60 more ticks = another 1.0 s (total 1.5 s) — transition must have fired
    for (int i = 30; i < 90; ++i) {
        sm.sample(s, static_cast<uint64_t>(i), 1.0 / 60.0);
    }
    CHECK(sm.currentState() == "b");
}

TEST_CASE("StateMachineController: child controller is recreated on re-entry") {
    SmFixture f;
    std::vector<glm::dvec3> wps = {
        glm::dvec3{0.0, 600.0, 0.0}, // wp0 — at entity position, captured immediately
        glm::dvec3{5000.0, 600.0, 0.0},
    };
    fl::ai::StateMachineController sm(f.em);
    sm.addState("patrol", [&wps] { return std::make_unique<fl::ai::WaypointController>(wps); });
    sm.addState("idle", [] { return std::make_unique<fl::ai::LoiterController>(glm::dvec3{}); });
    sm.addTransition("patrol", "idle", fl::ai::Always());
    sm.addTransition("idle", "patrol", fl::ai::Always());
    sm.setInitialState("patrol");

    fl::EntityState s = makeState(0.0, 600.0, 0.0);

    // Tick 0: capture wp0, advance to wp1; then transition fires to "idle".
    sm.sample(s, 0, 1.0 / 60.0);
    CHECK(sm.currentState() == "idle");

    // Tick 1: in "idle", transition fires back to "patrol"; new factory called.
    sm.sample(s, 1, 1.0 / 60.0);
    CHECK(sm.currentState() == "patrol");

    // Tick 2: sample the fresh WaypointController — it should be at index 0 (reset by factory).
    // Cast to inspect currentWaypointIndex — we need the fresh controller.
    // Since StateMachineController owns it, verify indirectly: at tick 2, patrol should
    // capture wp0 again (entity is still at origin) and advance to index 1.
    // The transition fires again to "idle" at end of tick 2.
    sm.sample(s, 2, 1.0 / 60.0);
    // If the factory hadn't been called, the old controller would be at wp1 and keep going;
    // but since wp0 is captured again (fresh start at index 0), the transition fires out.
    CHECK(sm.currentState() == "idle");
}

TEST_CASE("StateMachineController: skips self-transitions") {
    // Transition A->A: state name unchanged, dwell timer NOT reset.
    // Verify by adding A->B with minDwellSeconds=0.3; if self-transition reset the
    // timer, A->B would never fire within a short window.
    SmFixture f;
    fl::ai::StateMachineController sm(f.em);
    sm.addState("a", [] { return std::make_unique<fl::ai::LoiterController>(glm::dvec3{}); });
    sm.addState("b", [] { return std::make_unique<fl::ai::LoiterController>(glm::dvec3{}); });
    // Self-transition fires every tick but must not reset dwell.
    sm.addTransition("a", "a", fl::ai::Always());
    sm.addTransition("a", "b", fl::ai::Always(), /*minDwellSeconds=*/0.3f);
    sm.setInitialState("a");

    fl::EntityState s = makeState(0.0, 600.0, 0.0);

    // After 20 ticks (~0.33 s), the A->B transition should have fired despite the A->A.
    for (int i = 0; i < 20; ++i) {
        sm.sample(s, static_cast<uint64_t>(i), 1.0 / 60.0);
    }
    CHECK(sm.currentState() == "b");
}

TEST_CASE("StateMachineController: output on transition tick is from outgoing child") {
    // State A: LoiterController throttle=0.65; State B: WaypointController throttle=0.7.
    // Always() transition fires at end of tick 0. Output for tick 0 must be from A (0.65).
    SmFixture f;
    fl::ai::StateMachineController sm(f.em);
    sm.addState("a", [] {
        return std::make_unique<fl::ai::LoiterController>(glm::dvec3{0.0, 600.0, 0.0}, 3000.f, 600.f, 0.65f);
    });
    sm.addState("b", [] {
        return std::make_unique<fl::ai::WaypointController>(std::vector<glm::dvec3>{glm::dvec3{5000.0, 600.0, 0.0}},
                                                            500.f, 0.7f);
    });
    sm.addTransition("a", "b", fl::ai::Always());
    sm.setInitialState("a");

    fl::EntityState s = makeState(3000.0, 600.0, 0.0);
    fl::ControlInput inp = sm.sample(s, 0, 1.0 / 60.0); // transition fires, but output is A's
    CHECK(sm.currentState() == "b");
    CHECK(inp.throttle == Catch::Approx(0.65f).margin(1e-4f)); // A's throttle, not B's 0.7
}

// ---------------------------------------------------------------------------
// Built-in Condition helpers
// ---------------------------------------------------------------------------

TEST_CASE("ThreatWithinRange: true when target within range") {
    SmFixture f;
    fl::EntityState* tgt = f.em.get(f.targetId);
    REQUIRE(tgt != nullptr);
    tgt->transform.pos[2] = 4000.0; // 4 km away in Z

    fl::EntityState self = makeState(0.0, 0.0, 0.0);
    self.id = f.selfId;
    auto cond = fl::ai::ThreatWithinRange(f.targetId, 5000.f);
    CHECK(cond(self, f.em, nullptr) == true);
}

TEST_CASE("ThreatWithinRange: false when target beyond range") {
    SmFixture f;
    fl::EntityState* tgt = f.em.get(f.targetId);
    REQUIRE(tgt != nullptr);
    tgt->transform.pos[2] = 4000.0;

    fl::EntityState self = makeState(0.0, 0.0, 0.0);
    self.id = f.selfId;
    auto cond = fl::ai::ThreatWithinRange(f.targetId, 3000.f);
    CHECK(cond(self, f.em, nullptr) == false);
}

TEST_CASE("ThreatWithinRange: false when target is dead") {
    SmFixture f;
    fl::EntityState* tgt = f.em.get(f.targetId);
    REQUIRE(tgt != nullptr);
    tgt->transform.pos[2] = 100.0;

    f.em.kill(f.targetId);
    f.em.onTick(1.0 / 60.0, 1);

    fl::EntityState self = makeState(0.0, 0.0, 0.0);
    self.id = f.selfId;
    auto cond = fl::ai::ThreatWithinRange(f.targetId, 5000.f);
    CHECK(cond(self, f.em, nullptr) == false);
}

TEST_CASE("ThreatWithinRange: false for null EntityId") {
    SmFixture f;
    fl::EntityState self = makeState(0.0, 0.0, 0.0);
    self.id = f.selfId;
    auto cond = fl::ai::ThreatWithinRange(fl::EntityId::null(), 5000.f);
    CHECK(cond(self, f.em, nullptr) == false);
}

TEST_CASE("ThreatBeyondRange: false when target is alive and within range") {
    SmFixture f;
    fl::EntityState* tgt = f.em.get(f.targetId);
    REQUIRE(tgt != nullptr);
    tgt->transform.pos[2] = 2000.0;

    fl::EntityState self = makeState(0.0, 0.0, 0.0);
    self.id = f.selfId;
    auto cond = fl::ai::ThreatBeyondRange(f.targetId, 5000.f);
    CHECK(cond(self, f.em, nullptr) == false);
}

TEST_CASE("ThreatBeyondRange: true when target beyond range") {
    SmFixture f;
    fl::EntityState* tgt = f.em.get(f.targetId);
    REQUIRE(tgt != nullptr);
    tgt->transform.pos[2] = 8000.0;

    fl::EntityState self = makeState(0.0, 0.0, 0.0);
    self.id = f.selfId;
    auto cond = fl::ai::ThreatBeyondRange(f.targetId, 5000.f);
    CHECK(cond(self, f.em, nullptr) == true);
}

TEST_CASE("ThreatBeyondRange: true when target is dead") {
    SmFixture f;
    f.em.kill(f.targetId);
    f.em.onTick(1.0 / 60.0, 1);

    fl::EntityState self = makeState(0.0, 0.0, 0.0);
    self.id = f.selfId;
    auto cond = fl::ai::ThreatBeyondRange(f.targetId, 5000.f);
    CHECK(cond(self, f.em, nullptr) == true);
}

TEST_CASE("HpBelow: true when hp fraction below threshold") {
    SmFixture f;
    fl::EntityState self{};
    self.hp = 20.f;
    self.maxHp = 100.f;
    self.id = f.selfId;
    auto cond = fl::ai::HpBelow(0.25f);
    CHECK(cond(self, f.em, nullptr) == true);
}

TEST_CASE("HpBelow: false when hp fraction at or above threshold") {
    SmFixture f;
    fl::EntityState self{};
    self.hp = 30.f;
    self.maxHp = 100.f;
    self.id = f.selfId;
    auto cond = fl::ai::HpBelow(0.25f);
    CHECK(cond(self, f.em, nullptr) == false);
}

TEST_CASE("HpBelow: false when maxHp is zero") {
    SmFixture f;
    fl::EntityState self{};
    self.hp = 50.f;
    self.maxHp = 0.f;
    self.id = f.selfId;
    auto cond = fl::ai::HpBelow(0.5f);
    CHECK(cond(self, f.em, nullptr) == false);
}

TEST_CASE("AnyEntityWithinRange: true when si has a non-self neighbor") {
    SmFixture f;
    fl::SpatialIndex si;
    // Self at origin, neighbor at (0,0,500).
    double selfPos[3] = {0.0, 0.0, 0.0};
    double neighPos[3] = {0.0, 0.0, 500.0};
    si.insert(f.selfId.index, selfPos);
    si.insert(f.targetId.index, neighPos);

    fl::EntityState self{};
    self.id = f.selfId;
    self.transform.pos[0] = selfPos[0];
    self.transform.pos[1] = selfPos[1];
    self.transform.pos[2] = selfPos[2];

    auto cond = fl::ai::AnyEntityWithinRange(1000.f);
    CHECK(cond(self, f.em, &si) == true);
}

TEST_CASE("AnyEntityWithinRange: false when only self is within range") {
    SmFixture f;
    fl::SpatialIndex si;
    double selfPos[3] = {0.0, 0.0, 0.0};
    si.insert(f.selfId.index, selfPos); // only self

    fl::EntityState self{};
    self.id = f.selfId;
    self.transform.pos[0] = selfPos[0];
    self.transform.pos[1] = selfPos[1];
    self.transform.pos[2] = selfPos[2];

    auto cond = fl::ai::AnyEntityWithinRange(1000.f);
    CHECK(cond(self, f.em, &si) == false);
}

TEST_CASE("AnyEntityWithinRange: false when si has entities but all out of range") {
    SmFixture f;
    fl::SpatialIndex si;
    double selfPos[3] = {0.0, 0.0, 0.0};
    double farPos[3] = {0.0, 0.0, 5000.0};
    si.insert(f.selfId.index, selfPos);
    si.insert(f.targetId.index, farPos);

    fl::EntityState self{};
    self.id = f.selfId;
    self.transform.pos[0] = selfPos[0];
    self.transform.pos[1] = selfPos[1];
    self.transform.pos[2] = selfPos[2];

    auto cond = fl::ai::AnyEntityWithinRange(1000.f);
    CHECK(cond(self, f.em, &si) == false);
}

TEST_CASE("AnyEntityWithinRange: false when si is null") {
    SmFixture f;
    fl::EntityState self{};
    self.id = f.selfId;
    auto cond = fl::ai::AnyEntityWithinRange(1000.f);
    CHECK(cond(self, f.em, nullptr) == false);
}

TEST_CASE("Always: returns true") {
    SmFixture f;
    fl::EntityState self{};
    self.id = f.selfId;
    auto cond = fl::ai::Always();
    CHECK(cond(self, f.em, nullptr) == true);
}

TEST_CASE("And: true when both conditions true") {
    SmFixture f;
    fl::EntityState self{};
    self.id = f.selfId;
    auto cond = fl::ai::And(fl::ai::Always(), fl::ai::Always());
    CHECK(cond(self, f.em, nullptr) == true);
}

TEST_CASE("And: false when first condition false") {
    SmFixture f;
    fl::EntityState self{};
    self.id = f.selfId;
    auto cond = fl::ai::And(fl::ai::Not(fl::ai::Always()), fl::ai::Always());
    CHECK(cond(self, f.em, nullptr) == false);
}

TEST_CASE("And: false when second condition false") {
    SmFixture f;
    fl::EntityState self{};
    self.id = f.selfId;
    auto cond = fl::ai::And(fl::ai::Always(), fl::ai::Not(fl::ai::Always()));
    CHECK(cond(self, f.em, nullptr) == false);
}

TEST_CASE("Or: false when both conditions false") {
    SmFixture f;
    fl::EntityState self{};
    self.id = f.selfId;
    auto cond = fl::ai::Or(fl::ai::Not(fl::ai::Always()), fl::ai::Not(fl::ai::Always()));
    CHECK(cond(self, f.em, nullptr) == false);
}

TEST_CASE("Or: true when first condition true") {
    SmFixture f;
    fl::EntityState self{};
    self.id = f.selfId;
    auto cond = fl::ai::Or(fl::ai::Always(), fl::ai::Not(fl::ai::Always()));
    CHECK(cond(self, f.em, nullptr) == true);
}

TEST_CASE("Or: true when only second condition true") {
    SmFixture f;
    fl::EntityState self{};
    self.id = f.selfId;
    auto cond = fl::ai::Or(fl::ai::Not(fl::ai::Always()), fl::ai::Always());
    CHECK(cond(self, f.em, nullptr) == true);
}

TEST_CASE("Not: inverts true to false") {
    SmFixture f;
    fl::EntityState self{};
    self.id = f.selfId;
    auto cond = fl::ai::Not(fl::ai::Always());
    CHECK(cond(self, f.em, nullptr) == false);
}

TEST_CASE("Not: inverts false to true") {
    SmFixture f;
    fl::EntityState self{};
    self.id = f.selfId;
    auto cond = fl::ai::Not(fl::ai::Not(fl::ai::Always()));
    CHECK(cond(self, f.em, nullptr) == true);
}

// ---------------------------------------------------------------------------
// StateMachineController — integration: patrol-attack-retreat cycle
// ---------------------------------------------------------------------------

TEST_CASE("StateMachineController: full patrol-attack-retreat cycle") {
    SmFixture f;
    fl::EntityId attackerId = f.selfId;
    fl::EntityId targetId = f.targetId;

    // Place target far away initially (beyond 8 km engage threshold).
    fl::EntityState* tgt = f.em.get(targetId);
    REQUIRE(tgt != nullptr);
    tgt->transform.pos[2] = 10000.0;

    // Set attacker HP.
    fl::EntityState* atk = f.em.get(attackerId);
    REQUIRE(atk != nullptr);
    atk->hp = 100.f;
    atk->maxHp = 100.f;

    std::vector<glm::dvec3> wps = {glm::dvec3{1000.0, 600.0, 0.0}};
    fl::ai::StateMachineController sm(f.em);
    sm.addState("patrol", [&wps] { return std::make_unique<fl::ai::WaypointController>(wps, 500.f, 0.7f, true); });
    sm.addState("engage",
                [&f, targetId] { return std::make_unique<fl::ai::PursuitController>(f.em, targetId, 0.85f, true); });
    sm.addState("retreat", [&f, targetId] { return std::make_unique<fl::ai::EvadeController>(f.em, targetId); });
    sm.addTransition("patrol", "engage", fl::ai::ThreatWithinRange(targetId, 8000.f));
    sm.addTransition("engage", "retreat", fl::ai::HpBelow(0.25f));
    sm.addTransition("engage", "patrol", fl::ai::ThreatBeyondRange(targetId, 12000.f), 1.f);
    sm.addTransition("retreat", "engage",
                     fl::ai::And(fl::ai::Not(fl::ai::HpBelow(0.25f)), fl::ai::ThreatWithinRange(targetId, 6000.f)));
    sm.setInitialState("patrol");
    CHECK(sm.currentState() == "patrol");

    fl::EntityState selfState = makeState(0.0, 600.0, 0.0);
    selfState.id = attackerId;
    selfState.hp = 100.f;
    selfState.maxHp = 100.f;

    // Tick 0: target at 10 km — patrol stays.
    sm.sample(selfState, 0, 1.0 / 60.0);
    CHECK(sm.currentState() == "patrol");

    // Move target to 5 km — within engage threshold.
    tgt->transform.pos[2] = 5000.0;
    sm.sample(selfState, 1, 1.0 / 60.0);
    CHECK(sm.currentState() == "engage");

    // Drop HP below 25% — retreat fires.
    selfState.hp = 20.f;
    sm.sample(selfState, 2, 1.0 / 60.0);
    CHECK(sm.currentState() == "retreat");
}

// ---------------------------------------------------------------------------
// LeadPursuitController
// ---------------------------------------------------------------------------

namespace {
struct PursuitFixture {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    fl::EntityManager em;
    fl::EntityId attackerId;
    fl::EntityId targetId;

    PursuitFixture() : em(log, reg) {
        reg.registerType(makeBasicDef());
        fl::EntityTransform ta{};
        ta.quat[3] = 1.f; // identity: forward = +X
        attackerId = em.spawn("test:basic", ta);
        fl::EntityTransform tt{};
        tt.pos[2] = 2000.0; // 2 km to the right (+Z)
        tt.quat[3] = 1.f;
        targetId = em.spawn("test:basic", tt);
    }
};
} // namespace

TEST_CASE("LeadPursuitController: navGain=0 gives same aileron sign as PursuitController") {
    // With navGain=0 the lead point equals target.pos — identical to pure pursuit.
    PursuitFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::LeadPursuitController lead(f.em, f.targetId, /*navGain=*/0.f);
    fl::ai::PursuitController pure(f.em, f.targetId);

    fl::ControlInput leadInp = lead.sample(*as, 0, 1.0 / 60.0);
    fl::ControlInput pureInp = pure.sample(*as, 0, 1.0 / 60.0);

    // Both should bank right (target is at +Z).
    CHECK(leadInp.aileron > 0.f);
    CHECK(pureInp.aileron > 0.f);
}

TEST_CASE("LeadPursuitController: navGain=1 shifts aim ahead of a moving target") {
    // Target moves in +Z — with navGain=1 the lead point is further in +Z than target.pos,
    // so aileron command should be at least as large as pure pursuit for the same geometry.
    PursuitFixture f;
    fl::EntityState* tgt = f.em.get(f.targetId);
    REQUIRE(tgt != nullptr);
    tgt->transform.vel[2] = 100.f; // target moving right at 100 m/s

    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::LeadPursuitController lead(f.em, f.targetId, /*navGain=*/1.f);
    fl::ai::PursuitController pure(f.em, f.targetId);

    fl::ControlInput leadInp = lead.sample(*as, 0, 1.0 / 60.0);
    fl::ControlInput pureInp = pure.sample(*as, 0, 1.0 / 60.0);

    // Lead aims further right than pure pursuit.
    CHECK(leadInp.aileron >= pureInp.aileron);
    CHECK(leadInp.aileron > 0.f);
}

TEST_CASE("LeadPursuitController: co-located self and target does not crash") {
    // dist == 0: no lead computation; aims at target.pos (identical to own.pos); no NaN.
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(makeBasicDef());
    fl::EntityManager em(log, reg);

    fl::EntityTransform t{};
    t.quat[3] = 1.f;
    fl::EntityId attackerId = em.spawn("test:basic", t);
    fl::EntityId targetId = em.spawn("test:basic", t); // same position

    fl::ai::LeadPursuitController ctrl(em, targetId, 1.f);
    const fl::EntityState* as = em.get(attackerId);
    REQUIRE(as != nullptr);
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    // Controls may be anything non-NaN; just verify no crash and throttle is set.
    CHECK(ctrl.sample(*as, 1, 1.0 / 60.0).throttle > 0.f);
    (void)inp; // suppress unused warning
}

TEST_CASE("LeadPursuitController: zero relative velocity clamps TTC floor") {
    // Self and target stationary (vel = 0): closingSpeed <= 0 → clamped to 10 m/s; no divide by zero.
    PursuitFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::LeadPursuitController ctrl(f.em, f.targetId, 1.f);
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.throttle > 0.f);
    CHECK(inp.aileron > 0.f); // still steers toward target
}

TEST_CASE("LeadPursuitController: returns neutral when target is dead") {
    PursuitFixture f;
    f.em.kill(f.targetId);
    f.em.onTick(1.0 / 60.0, 1);

    fl::ai::LeadPursuitController ctrl(f.em, f.targetId);
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);
    fl::ControlInput inp = ctrl.sample(*as, 1, 1.0 / 60.0);

    CHECK(inp.throttle == Catch::Approx(0.f));
    CHECK(inp.aileron == Catch::Approx(0.f));
}

TEST_CASE("LeadPursuitController: returns neutral for invalid EntityId") {
    PursuitFixture f;
    fl::ai::LeadPursuitController ctrl(f.em, fl::EntityId::null());
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.throttle == Catch::Approx(0.f));
    CHECK(inp.aileron == Catch::Approx(0.f));
}

TEST_CASE("LeadPursuitController: setTarget flips aileron for opposite-side target") {
    PursuitFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    // Spawn a second target on the left (-Z).
    fl::EntityTransform tl{};
    tl.pos[2] = -2000.0;
    tl.quat[3] = 1.f;
    fl::EntityId leftId = f.em.spawn("test:basic", tl);

    fl::ai::LeadPursuitController ctrl(f.em, f.targetId); // right target → positive aileron
    fl::ControlInput right = ctrl.sample(*as, 0, 1.0 / 60.0);
    CHECK(right.aileron > 0.f);

    ctrl.setTarget(leftId); // swap to left target → negative aileron
    fl::ControlInput left = ctrl.sample(*as, 1, 1.0 / 60.0);
    CHECK(left.aileron < 0.f);
}

TEST_CASE("LeadPursuitController: throttle and afterburner propagate from constructor") {
    PursuitFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::LeadPursuitController ctrl(f.em, f.targetId, 1.f, /*throttle=*/0.7f, /*useAfterburner=*/true);
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.throttle == Catch::Approx(0.7f).margin(1e-5f));
    CHECK(inp.afterburner == true);
}

// ---------------------------------------------------------------------------
// LagPursuitController
// ---------------------------------------------------------------------------

TEST_CASE("LagPursuitController: lagFraction=0 gives same aileron sign as PursuitController") {
    // With lagFraction=0 the lag point equals target.pos — identical to pure pursuit.
    PursuitFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::LagPursuitController lag(f.em, f.targetId, /*lagFraction=*/0.f);
    fl::ai::PursuitController pure(f.em, f.targetId);

    fl::ControlInput lagInp = lag.sample(*as, 0, 1.0 / 60.0);
    fl::ControlInput pureInp = pure.sample(*as, 0, 1.0 / 60.0);

    // Both should bank right (target is at +Z).
    CHECK(lagInp.aileron > 0.f);
    CHECK(pureInp.aileron > 0.f);
}

TEST_CASE("LagPursuitController: lagFraction=1 shifts aim behind a moving target") {
    // Target at +Z 2000m moving right (+Z) at 100 m/s; attacker stationary.
    // closing speed < 0 → floored to 10 → TTC capped to 30 s
    // lag point z = 2000 - 100 * 30 * 1 = -1000 (behind attacker → aileron < 0)
    // pure pursuit aims at +Z 2000m → aileron > 0
    PursuitFixture f;
    fl::EntityState* tgt = f.em.get(f.targetId);
    REQUIRE(tgt != nullptr);
    tgt->transform.vel[2] = 100.f;

    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::LagPursuitController lag(f.em, f.targetId, /*lagFraction=*/1.f);
    fl::ai::PursuitController pure(f.em, f.targetId);

    fl::ControlInput lagInp = lag.sample(*as, 0, 1.0 / 60.0);
    fl::ControlInput pureInp = pure.sample(*as, 0, 1.0 / 60.0);

    // Lag aims further behind than pure pursuit.
    CHECK(lagInp.aileron < pureInp.aileron);
}

TEST_CASE("LagPursuitController: lagFraction=0.5 gives intermediate aim between pure and full lag") {
    // Target at (1000, 0, 500) moving +Z at 10 m/s; attacker at origin facing +X.
    // closing speed < 0 → floored to 10 → TTC = 30 s.
    // lag points (all X-shifted only in Z): lag05=(1000,0,350), lag1=(1000,0,200), pure=(1000,0,500).
    // All heading errors are in the linear range of bankToTurnAileron (< 90°), giving strict ordering.
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(makeBasicDef());
    fl::EntityManager em(log, reg);

    fl::EntityTransform ta{};
    ta.quat[3] = 1.f; // identity: forward = +X
    fl::EntityId attackerId = em.spawn("test:basic", ta);
    fl::EntityTransform tt{};
    tt.pos[0] = 1000.0;
    tt.pos[2] = 500.0;
    tt.vel[2] = 10.f;
    tt.quat[3] = 1.f;
    fl::EntityId targetId = em.spawn("test:basic", tt);

    const fl::EntityState* as = em.get(attackerId);
    REQUIRE(as != nullptr);

    fl::ai::LagPursuitController lag05(em, targetId, /*lagFraction=*/0.5f);
    fl::ai::LagPursuitController lag1(em, targetId, /*lagFraction=*/1.f);
    fl::ai::PursuitController pure(em, targetId);

    fl::ControlInput inp05 = lag05.sample(*as, 0, 1.0 / 60.0);
    fl::ControlInput inp1 = lag1.sample(*as, 0, 1.0 / 60.0);
    fl::ControlInput inpPure = pure.sample(*as, 0, 1.0 / 60.0);

    // lag1 aims further behind → smaller (or more negative) aileron than lag05 and pure.
    CHECK(inp1.aileron < inp05.aileron);
    CHECK(inp05.aileron < inpPure.aileron);
}

TEST_CASE("LagPursuitController: lagFraction=2 shifts aim further behind than lagFraction=1") {
    // Same geometry: target at (1000, 0, 500), vel[2]=10 m/s, TTC=30 s.
    // lag1 z = 500 - 10*30*1 = 200; lag2 z = 500 - 10*30*2 = -100.
    // lag2 aims further behind → smaller aileron than lag1.
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(makeBasicDef());
    fl::EntityManager em(log, reg);

    fl::EntityTransform ta{};
    ta.quat[3] = 1.f;
    fl::EntityId attackerId = em.spawn("test:basic", ta);
    fl::EntityTransform tt{};
    tt.pos[0] = 1000.0;
    tt.pos[2] = 500.0;
    tt.vel[2] = 10.f;
    tt.quat[3] = 1.f;
    fl::EntityId targetId = em.spawn("test:basic", tt);

    const fl::EntityState* as = em.get(attackerId);
    REQUIRE(as != nullptr);

    fl::ai::LagPursuitController lag1(em, targetId, /*lagFraction=*/1.f);
    fl::ai::LagPursuitController lag2(em, targetId, /*lagFraction=*/2.f);

    fl::ControlInput inp1 = lag1.sample(*as, 0, 1.0 / 60.0);
    fl::ControlInput inp2 = lag2.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp2.aileron < inp1.aileron);
}

TEST_CASE("LagPursuitController: zero relative velocity clamps TTC floor") {
    // Both stationary; vel=0 → lag point = target.pos → same as pure pursuit, no NaN.
    PursuitFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::LagPursuitController ctrl(f.em, f.targetId, /*lagFraction=*/1.f);
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.throttle > 0.f);
    CHECK(inp.aileron > 0.f); // target at +Z; vel=0 so lag point = target.pos, still to the right
}

TEST_CASE("LagPursuitController: co-located self and target does not crash") {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(makeBasicDef());
    fl::EntityManager em(log, reg);

    fl::EntityTransform t{};
    t.quat[3] = 1.f;
    fl::EntityId attackerId = em.spawn("test:basic", t);
    fl::EntityId targetId = em.spawn("test:basic", t); // same position

    fl::ai::LagPursuitController ctrl(em, targetId, 1.f);
    const fl::EntityState* as = em.get(attackerId);
    REQUIRE(as != nullptr);

    // No crash; throttle is applied.
    CHECK(ctrl.sample(*as, 0, 1.0 / 60.0).throttle > 0.f);
    CHECK(ctrl.sample(*as, 1, 1.0 / 60.0).throttle > 0.f);
}

TEST_CASE("LagPursuitController: returns neutral when target is dead") {
    PursuitFixture f;
    f.em.kill(f.targetId);
    f.em.onTick(1.0 / 60.0, 1);

    fl::ai::LagPursuitController ctrl(f.em, f.targetId);
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);
    fl::ControlInput inp = ctrl.sample(*as, 1, 1.0 / 60.0);

    CHECK(inp.throttle == Catch::Approx(0.f));
    CHECK(inp.aileron == Catch::Approx(0.f));
}

TEST_CASE("LagPursuitController: returns neutral for invalid EntityId") {
    PursuitFixture f;
    fl::ai::LagPursuitController ctrl(f.em, fl::EntityId::null());
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.throttle == Catch::Approx(0.f));
    CHECK(inp.aileron == Catch::Approx(0.f));
}

TEST_CASE("LagPursuitController: setTarget flips aileron for opposite-side target") {
    PursuitFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    // Spawn a second target on the left (-Z).
    fl::EntityTransform tl{};
    tl.pos[2] = -2000.0;
    tl.quat[3] = 1.f;
    fl::EntityId leftId = f.em.spawn("test:basic", tl);

    fl::ai::LagPursuitController ctrl(f.em, f.targetId); // right target → positive aileron
    fl::ControlInput right = ctrl.sample(*as, 0, 1.0 / 60.0);
    CHECK(right.aileron > 0.f);

    ctrl.setTarget(leftId); // swap to left target → negative aileron
    fl::ControlInput left = ctrl.sample(*as, 1, 1.0 / 60.0);
    CHECK(left.aileron < 0.f);
}

TEST_CASE("LagPursuitController: throttle and afterburner propagate from constructor") {
    PursuitFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::LagPursuitController ctrl(f.em, f.targetId, 1.f, /*throttle=*/0.7f, /*useAfterburner=*/true);
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.throttle == Catch::Approx(0.7f).margin(1e-5f));
    CHECK(inp.afterburner == true);
}

// ---------------------------------------------------------------------------
// ImmelmannController
// ---------------------------------------------------------------------------

TEST_CASE("ImmelmannController: Pull phase has full elevator and afterburner") {
    fl::ai::ImmelmannController ctrl;
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    fl::ControlInput inp = ctrl.sample(s, 0, 1.0 / 60.0); // well within Pull duration

    CHECK(inp.elevator == Catch::Approx(1.f).margin(1e-5f));
    CHECK(inp.throttle == Catch::Approx(1.f).margin(1e-5f));
    CHECK(inp.afterburner == true);
    CHECK(inp.aileron == Catch::Approx(0.f).margin(1e-5f));
}

TEST_CASE("ImmelmannController: partial dt stays in Pull phase") {
    fl::ai::ImmelmannController ctrl(4.0f, 1.5f);
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    fl::ControlInput inp = ctrl.sample(s, 0, 2.0); // half of 4 s pull duration

    CHECK(inp.elevator == Catch::Approx(1.f).margin(1e-5f));
    CHECK(inp.aileron == Catch::Approx(0.f).margin(1e-5f));
}

TEST_CASE("ImmelmannController: transitions to Roll after Pull duration") {
    fl::ai::ImmelmannController ctrl(4.0f, 1.5f);
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    fl::ControlInput inp = ctrl.sample(s, 0, 4.0); // exactly Pull duration → Roll

    CHECK(inp.aileron > 0.f);
    CHECK(inp.elevator == Catch::Approx(0.f).margin(1e-5f));
    CHECK(inp.afterburner == true);
}

TEST_CASE("ImmelmannController: transitions to Done after Roll duration") {
    fl::ai::ImmelmannController ctrl(4.0f, 1.5f);
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    ctrl.sample(s, 0, 4.0);                        // → Roll
    fl::ControlInput inp = ctrl.sample(s, 1, 1.5); // → Done

    CHECK(inp.elevator == Catch::Approx(0.f).margin(1e-5f));
    CHECK(inp.aileron == Catch::Approx(0.f).margin(1e-5f));
    CHECK(inp.throttle == Catch::Approx(0.f).margin(1e-5f));
    CHECK(inp.afterburner == false);
}

TEST_CASE("ImmelmannController: Done phase returns neutral on repeated calls") {
    fl::ai::ImmelmannController ctrl(4.0f, 1.5f);
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    ctrl.sample(s, 0, 4.0); // → Roll
    ctrl.sample(s, 1, 1.5); // → Done

    for (uint64_t i = 2; i < 5; ++i) {
        fl::ControlInput inp = ctrl.sample(s, i, 1.0 / 60.0);
        CHECK(inp.elevator == Catch::Approx(0.f).margin(1e-5f));
        CHECK(inp.aileron == Catch::Approx(0.f).margin(1e-5f));
        CHECK(inp.throttle == Catch::Approx(0.f).margin(1e-5f));
    }
}

TEST_CASE("ImmelmannController: default-constructed timing produces expected phase sequence") {
    fl::ai::ImmelmannController ctrl; // pullDuration=4.0, rollDuration=1.5
    fl::EntityState s = makeState(0.0, 600.0, 0.0);

    // Pull phase: elevator up.
    fl::ControlInput pull = ctrl.sample(s, 0, 0.1);
    CHECK(pull.elevator > 0.f);

    // Fast-forward to Roll.
    ctrl.sample(s, 1, 4.0);
    fl::ControlInput roll = ctrl.sample(s, 2, 0.1);
    CHECK(roll.aileron > 0.f);
    CHECK(roll.elevator == Catch::Approx(0.f).margin(1e-5f));
}

// ---------------------------------------------------------------------------
// SplitSController
// ---------------------------------------------------------------------------

TEST_CASE("SplitSController: Roll phase has aileron, speedbrake, and idle throttle") {
    fl::ai::SplitSController ctrl;
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    fl::ControlInput inp = ctrl.sample(s, 0, 1.0 / 60.0);

    CHECK(inp.aileron > 0.f);
    CHECK(inp.speedbrake > 0.f); // explicit: speedbrake field is set
    CHECK(inp.throttle == Catch::Approx(0.f).margin(1e-5f));
    CHECK(inp.elevator == Catch::Approx(0.f).margin(1e-5f));
}

TEST_CASE("SplitSController: Roll phase elevator is neutral") {
    fl::ai::SplitSController ctrl;
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    fl::ControlInput inp = ctrl.sample(s, 0, 0.5); // still in Roll (default 1.5 s)

    CHECK(inp.elevator == Catch::Approx(0.f).margin(1e-5f));
}

TEST_CASE("SplitSController: transitions to Pull after Roll duration") {
    fl::ai::SplitSController ctrl(1.5f, 4.0f);
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    fl::ControlInput inp = ctrl.sample(s, 0, 1.5); // → Pull

    CHECK(inp.elevator == Catch::Approx(1.f).margin(1e-5f));
    CHECK(inp.throttle == Catch::Approx(1.f).margin(1e-5f));
    CHECK(inp.speedbrake == Catch::Approx(0.f).margin(1e-5f));
}

TEST_CASE("SplitSController: transitions to Done after Pull duration") {
    fl::ai::SplitSController ctrl(1.5f, 4.0f);
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    ctrl.sample(s, 0, 1.5);                        // → Pull
    fl::ControlInput inp = ctrl.sample(s, 1, 4.0); // → Done

    CHECK(inp.elevator == Catch::Approx(0.f).margin(1e-5f));
    CHECK(inp.aileron == Catch::Approx(0.f).margin(1e-5f));
    CHECK(inp.throttle == Catch::Approx(0.f).margin(1e-5f));
    CHECK(inp.speedbrake == Catch::Approx(0.f).margin(1e-5f));
}

TEST_CASE("SplitSController: Done phase returns neutral on repeated calls") {
    fl::ai::SplitSController ctrl(1.5f, 4.0f);
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    ctrl.sample(s, 0, 1.5); // → Pull
    ctrl.sample(s, 1, 4.0); // → Done

    for (uint64_t i = 2; i < 5; ++i) {
        fl::ControlInput inp = ctrl.sample(s, i, 1.0 / 60.0);
        CHECK(inp.elevator == Catch::Approx(0.f).margin(1e-5f));
        CHECK(inp.aileron == Catch::Approx(0.f).margin(1e-5f));
        CHECK(inp.speedbrake == Catch::Approx(0.f).margin(1e-5f));
    }
}

TEST_CASE("SplitSController: partial dt stays in Roll phase") {
    fl::ai::SplitSController ctrl(1.5f, 4.0f);
    fl::EntityState s = makeState(0.0, 600.0, 0.0);
    fl::ControlInput inp = ctrl.sample(s, 0, 0.75); // half of 1.5 s

    CHECK(inp.aileron > 0.f);                                // still rolling
    CHECK(inp.elevator == Catch::Approx(0.f).margin(1e-5f)); // not pulling yet
}

// ---------------------------------------------------------------------------
// HighYoYoController
// ---------------------------------------------------------------------------

namespace {
struct YoYoFixture {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    fl::EntityManager em;
    fl::EntityId attackerId;
    fl::EntityId targetId;

    YoYoFixture() : em(log, reg) {
        reg.registerType(makeBasicDef());
        fl::EntityTransform ta{};
        ta.quat[3] = 1.f; // forward = +X
        attackerId = em.spawn("test:basic", ta);
        fl::EntityTransform tt{};
        tt.pos[2] = 2000.0; // 2 km to the right (+Z)
        tt.quat[3] = 1.f;
        targetId = em.spawn("test:basic", tt);
    }
};
} // namespace

TEST_CASE("HighYoYoController: Climb phase banks AWAY from target") {
    // Target is to the right (+Z) → PursuitController banks right (aileron > 0).
    // HighYoYo Climb must bank away → aileron < 0.
    YoYoFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::HighYoYoController ctrl(f.em, f.targetId, 10.f, 3.0f); // long climb so we stay in it
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    fl::ai::PursuitController pure(f.em, f.targetId);
    fl::ControlInput pureInp = pure.sample(*as, 0, 1.0 / 60.0);

    CHECK(pureInp.aileron > 0.f); // pure pursuit banks right
    CHECK(inp.aileron < 0.f);     // HighYoYo Climb banks left (away)
}

TEST_CASE("HighYoYoController: Climb phase elevator is positive") {
    YoYoFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::HighYoYoController ctrl(f.em, f.targetId, 10.f, 3.0f);
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.elevator > 0.f); // pulling up
}

TEST_CASE("HighYoYoController: Climb phase throttle is less than 1") {
    YoYoFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::HighYoYoController ctrl(f.em, f.targetId, 10.f, 3.0f);
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.throttle < 1.f); // reduced power to bleed speed
    CHECK(inp.throttle > 0.f);
}

TEST_CASE("HighYoYoController: Reacquire phase banks TOWARD target") {
    YoYoFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::HighYoYoController ctrl(f.em, f.targetId, 0.0f, 10.f); // zero climb → enter Reacquire immediately
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.aileron > 0.f); // target is right → bank right (toward)
}

TEST_CASE("HighYoYoController: phase transition at correct accumulated dt") {
    YoYoFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::HighYoYoController ctrl(f.em, f.targetId, 2.5f, 3.0f);

    // Well within Climb: aileron < 0 (away from right-side target).
    fl::ControlInput climb = ctrl.sample(*as, 0, 0.5);
    CHECK(climb.aileron < 0.f);

    // Cross Climb threshold: enter Reacquire; aileron > 0 (toward).
    fl::ControlInput reacq = ctrl.sample(*as, 1, 2.5);
    CHECK(reacq.aileron > 0.f);
}

TEST_CASE("HighYoYoController: returns neutral when target dead in Climb phase") {
    YoYoFixture f;
    f.em.kill(f.targetId);
    f.em.onTick(1.0 / 60.0, 1);

    fl::ai::HighYoYoController ctrl(f.em, f.targetId, 10.f, 3.0f);
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);
    fl::ControlInput inp = ctrl.sample(*as, 1, 1.0 / 60.0);

    CHECK(inp.throttle == Catch::Approx(0.f));
    CHECK(inp.aileron == Catch::Approx(0.f));
    CHECK(inp.elevator == Catch::Approx(0.f));
}

TEST_CASE("HighYoYoController: returns neutral when target dead in Reacquire phase") {
    YoYoFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::HighYoYoController ctrl(f.em, f.targetId, 0.0f, 10.f); // skip to Reacquire immediately
    ctrl.sample(*as, 0, 1.0 / 60.0);                               // enter Reacquire

    f.em.kill(f.targetId);
    f.em.onTick(1.0 / 60.0, 1);

    fl::ControlInput inp = ctrl.sample(*as, 1, 1.0 / 60.0);
    CHECK(inp.throttle == Catch::Approx(0.f));
    CHECK(inp.aileron == Catch::Approx(0.f));
}

// ---------------------------------------------------------------------------
// LowYoYoController
// ---------------------------------------------------------------------------

TEST_CASE("LowYoYoController: Dive phase has negative elevator (unloading)") {
    YoYoFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::LowYoYoController ctrl(f.em, f.targetId, 10.f, 2.5f); // long dive
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.elevator < 0.f); // unloading to descend
}

TEST_CASE("LowYoYoController: Dive phase steers toward target") {
    YoYoFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::LowYoYoController ctrl(f.em, f.targetId, 10.f, 2.5f);
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.aileron > 0.f); // target is to the right → bank right
}

TEST_CASE("LowYoYoController: Dive phase has full throttle and afterburner") {
    YoYoFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::LowYoYoController ctrl(f.em, f.targetId, 10.f, 2.5f);
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.throttle == Catch::Approx(1.f).margin(1e-5f));
    CHECK(inp.afterburner == true);
}

TEST_CASE("LowYoYoController: Pull phase has positive elevator") {
    YoYoFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::LowYoYoController ctrl(f.em, f.targetId, 0.0f, 10.f); // zero dive → enter Pull immediately
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.elevator > 0.f); // pulling up
}

TEST_CASE("LowYoYoController: Pull phase continues to steer toward target") {
    YoYoFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::LowYoYoController ctrl(f.em, f.targetId, 0.0f, 10.f); // start in Pull
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.aileron > 0.f); // target still to the right
}

TEST_CASE("LowYoYoController: phase transition at correct accumulated dt") {
    YoYoFixture f;
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);

    fl::ai::LowYoYoController ctrl(f.em, f.targetId, 1.5f, 2.5f);

    // Dive phase: elevator < 0.
    fl::ControlInput dive = ctrl.sample(*as, 0, 0.5);
    CHECK(dive.elevator < 0.f);

    // Cross Dive threshold: enter Pull; elevator > 0.
    fl::ControlInput pull = ctrl.sample(*as, 1, 1.5);
    CHECK(pull.elevator > 0.f);
}

TEST_CASE("LowYoYoController: returns neutral when target dead in Dive phase") {
    YoYoFixture f;
    f.em.kill(f.targetId);
    f.em.onTick(1.0 / 60.0, 1);

    fl::ai::LowYoYoController ctrl(f.em, f.targetId, 10.f, 2.5f);
    const fl::EntityState* as = f.em.get(f.attackerId);
    REQUIRE(as != nullptr);
    fl::ControlInput inp = ctrl.sample(*as, 1, 1.0 / 60.0);

    CHECK(inp.throttle == Catch::Approx(0.f));
    CHECK(inp.aileron == Catch::Approx(0.f));
    CHECK(inp.elevator == Catch::Approx(0.f));
}

// ---------------------------------------------------------------------------
// AiControllerFactory — new behaviors
// ---------------------------------------------------------------------------

TEST_CASE("AiControllerFactory: lead from string args") {
    FactoryFixture f;
    std::string idxStr = std::to_string(f.entityId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("lead", std::span{args}, &f.em);
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: lead with navGain arg") {
    FactoryFixture f;
    std::string idxStr = std::to_string(f.entityId.index);
    std::vector<std::string_view> args = {idxStr, "2.0"};
    auto ctrl = fl::ai::createController("lead", std::span{args}, &f.em);
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: lead with missing entityIdx returns nullptr") {
    FactoryFixture f;
    std::vector<std::string_view> args;
    auto ctrl = fl::ai::createController("lead", std::span{args}, &f.em);
    CHECK(ctrl == nullptr);
}

TEST_CASE("AiControllerFactory: lead without entityManager returns nullptr") {
    std::vector<std::string_view> args = {"0"};
    auto ctrl = fl::ai::createController("lead", std::span{args}, nullptr);
    CHECK(ctrl == nullptr);
}

TEST_CASE("AiControllerFactory: lead with nonexistent entity returns nullptr") {
    FactoryFixture f;
    std::vector<std::string_view> args = {"9999"};
    auto ctrl = fl::ai::createController("lead", std::span{args}, &f.em);
    CHECK(ctrl == nullptr);
}

TEST_CASE("AiControllerFactory: lead with invalid navGain arg returns nullptr") {
    FactoryFixture f;
    std::string idxStr = std::to_string(f.entityId.index);
    std::vector<std::string_view> args = {idxStr, "not_a_float"};
    auto ctrl = fl::ai::createController("lead", std::span{args}, &f.em);
    CHECK(ctrl == nullptr);
}

TEST_CASE("AiControllerFactory: lag from string args") {
    FactoryFixture f;
    std::string idxStr = std::to_string(f.entityId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("lag", std::span{args}, &f.em);
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: lag with lagFraction arg") {
    FactoryFixture f;
    std::string idxStr = std::to_string(f.entityId.index);
    std::vector<std::string_view> args = {idxStr, "0.5"};
    auto ctrl = fl::ai::createController("lag", std::span{args}, &f.em);
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: lag with missing entityIdx returns nullptr") {
    FactoryFixture f;
    std::vector<std::string_view> args;
    auto ctrl = fl::ai::createController("lag", std::span{args}, &f.em);
    CHECK(ctrl == nullptr);
}

TEST_CASE("AiControllerFactory: lag without entityManager returns nullptr") {
    std::vector<std::string_view> args = {"0"};
    auto ctrl = fl::ai::createController("lag", std::span{args}, nullptr);
    CHECK(ctrl == nullptr);
}

TEST_CASE("AiControllerFactory: lag with nonexistent entity returns nullptr") {
    FactoryFixture f;
    std::vector<std::string_view> args = {"9999"};
    auto ctrl = fl::ai::createController("lag", std::span{args}, &f.em);
    CHECK(ctrl == nullptr);
}

TEST_CASE("AiControllerFactory: lag with invalid lagFraction arg returns nullptr") {
    FactoryFixture f;
    std::string idxStr = std::to_string(f.entityId.index);
    std::vector<std::string_view> args = {idxStr, "bad_float"};
    auto ctrl = fl::ai::createController("lag", std::span{args}, &f.em);
    CHECK(ctrl == nullptr);
}

TEST_CASE("AiControllerFactory: immelmann with no args uses defaults") {
    std::vector<std::string_view> args;
    auto ctrl = fl::ai::createController("immelmann", std::span{args});
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: immelmann without entityManager still works") {
    std::vector<std::string_view> args;
    auto ctrl = fl::ai::createController("immelmann", std::span{args}, nullptr);
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: immelmann with invalid float arg returns nullptr") {
    std::vector<std::string_view> args = {"bad_float"};
    auto ctrl = fl::ai::createController("immelmann", std::span{args});
    CHECK(ctrl == nullptr);
}

TEST_CASE("AiControllerFactory: split_s with no args uses defaults") {
    std::vector<std::string_view> args;
    auto ctrl = fl::ai::createController("split_s", std::span{args});
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: split_s with invalid float arg returns nullptr") {
    std::vector<std::string_view> args = {"bad_float"};
    auto ctrl = fl::ai::createController("split_s", std::span{args});
    CHECK(ctrl == nullptr);
}

TEST_CASE("AiControllerFactory: high_yo_yo from string args") {
    FactoryFixture f;
    std::string idxStr = std::to_string(f.entityId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("high_yo_yo", std::span{args}, &f.em);
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: high_yo_yo with missing entityIdx returns nullptr") {
    FactoryFixture f;
    std::vector<std::string_view> args;
    auto ctrl = fl::ai::createController("high_yo_yo", std::span{args}, &f.em);
    CHECK(ctrl == nullptr);
}

TEST_CASE("AiControllerFactory: high_yo_yo without entityManager returns nullptr") {
    std::vector<std::string_view> args = {"0"};
    auto ctrl = fl::ai::createController("high_yo_yo", std::span{args}, nullptr);
    CHECK(ctrl == nullptr);
}

TEST_CASE("AiControllerFactory: low_yo_yo from string args") {
    FactoryFixture f;
    std::string idxStr = std::to_string(f.entityId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("low_yo_yo", std::span{args}, &f.em);
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: low_yo_yo with missing entityIdx returns nullptr") {
    FactoryFixture f;
    std::vector<std::string_view> args;
    auto ctrl = fl::ai::createController("low_yo_yo", std::span{args}, &f.em);
    CHECK(ctrl == nullptr);
}

// ---------------------------------------------------------------------------
// Integration: LeadPursuitController + ImmelmannController via StateMachineController
// ---------------------------------------------------------------------------

TEST_CASE("StateMachineController: lead-pursuit to Immelmann on dwell timeout") {
    SmFixture f;

    // Place target to the right so LeadPursuit produces a non-zero aileron.
    fl::EntityState* tgt = f.em.get(f.targetId);
    REQUIRE(tgt != nullptr);
    tgt->transform.pos[2] = 3000.0;

    fl::ai::StateMachineController sm(f.em);
    fl::EntityId targetId = f.targetId;
    sm.addState("engage", [&f, targetId] { return std::make_unique<fl::ai::LeadPursuitController>(f.em, targetId); });
    sm.addState("immelmann", [] { return std::make_unique<fl::ai::ImmelmannController>(4.0f, 1.5f); });
    // Transition after 0.5 s dwell in engage.
    sm.addTransition("engage", "immelmann", fl::ai::Always(), /*minDwellSeconds=*/0.5f);
    sm.setInitialState("engage");

    fl::EntityState selfState = makeState(0.0, 600.0, 0.0);
    selfState.id = f.selfId;

    // Tick 0: still in engage (dwell not yet met); output should be from LeadPursuit.
    fl::ControlInput engageOut = sm.sample(selfState, 0, 1.0 / 60.0);
    CHECK(sm.currentState() == "engage");
    CHECK(engageOut.throttle > 0.f); // LeadPursuit applies throttle

    // Tick after 0.5 s total: transition to Immelmann.
    for (int i = 1; i <= 30; ++i) {
        sm.sample(selfState, static_cast<uint64_t>(i), 1.0 / 60.0);
    }
    CHECK(sm.currentState() == "immelmann");

    // Next output must be ImmelmannController's Pull phase: elevator=1.
    fl::ControlInput immelOut = sm.sample(selfState, 31, 1.0 / 60.0);
    CHECK(immelOut.elevator == Catch::Approx(1.f).margin(1e-5f));
}

// ---------------------------------------------------------------------------
// Integration: LeadPursuitController → LagPursuitController via StateMachineController
// This is the canonical use case from issue #432: transition from lead to lag
// when inside the target's turn circle (within 3000 m).
// ---------------------------------------------------------------------------

TEST_CASE("StateMachineController: lead-to-lag transition on ThreatWithinRange") {
    SmFixture f;

    // Place target 2000 m to the right (+Z) — within the 3000 m transition threshold.
    fl::EntityState* tgt = f.em.get(f.targetId);
    REQUIRE(tgt != nullptr);
    tgt->transform.pos[2] = 2000.0;

    fl::ai::StateMachineController sm(f.em);
    fl::EntityId targetId = f.targetId;
    sm.addState("lead", [&f, targetId] { return std::make_unique<fl::ai::LeadPursuitController>(f.em, targetId); });
    sm.addState("lag", [&f, targetId] { return std::make_unique<fl::ai::LagPursuitController>(f.em, targetId); });
    // Transition immediately (no dwell) when within 3000 m.
    sm.addTransition("lead", "lag", fl::ai::ThreatWithinRange(targetId, 3000.f));
    sm.setInitialState("lead");

    fl::EntityState selfState = makeState(0.0, 0.0, 0.0);
    selfState.id = f.selfId;

    // Tick 0: sample-first — output comes from LeadPursuitController (throttle 0.9f default),
    // then transition fires because dist=2000 < 3000.
    fl::ControlInput firstOut = sm.sample(selfState, 0, 1.0 / 60.0);
    CHECK(sm.currentState() == "lag");
    CHECK(firstOut.throttle == Catch::Approx(0.9f).margin(1e-5f)); // LeadPursuit default

    // Tick 1: LagPursuitController is now active (throttle 0.85f default).
    fl::ControlInput lagOut = sm.sample(selfState, 1, 1.0 / 60.0);
    CHECK(sm.currentState() == "lag");
    CHECK(lagOut.throttle == Catch::Approx(0.85f).margin(1e-5f)); // LagPursuit default
    CHECK(lagOut.aileron > 0.f); // target at +Z; vel=0 so lag point = target.pos, still right
}

// ---------------------------------------------------------------------------
// AiControllerFactory — patrol_attack and escort templates
// ---------------------------------------------------------------------------

namespace {

// Pool slot 0 = selfId (attacker, at origin), slot 1 = targetId (1 km ahead).
struct PatrolAttackFixture {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    fl::EntityManager em;
    fl::EntityId selfId;
    fl::EntityId targetId;

    PatrolAttackFixture() : em(log, reg) {
        reg.registerType(makeBasicDef());
        fl::EntityTransform ta{};
        ta.quat[3] = 1.f;
        selfId = em.spawn("test:basic", ta); // index=0, at origin
        fl::EntityTransform tt{};
        tt.pos[0] = 1000.0; // 1 km ahead (within default 8000 m engage range)
        tt.quat[3] = 1.f;
        targetId = em.spawn("test:basic", tt); // index=1
    }
};

// Pool slot 0 = selfId (escort, at origin), slot 1 = escorteeId (2 km away),
// slot 2 = threatId (400 m away, within standoffM*0.5=1000 m).
struct EscortFixture {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    fl::EntityManager em;
    fl::EntityId selfId;
    fl::EntityId escorteeId;
    fl::EntityId threatId;

    EscortFixture() : em(log, reg) {
        reg.registerType(makeBasicDef());
        fl::EntityTransform ts{};
        ts.quat[3] = 1.f;
        selfId = em.spawn("test:basic", ts); // index=0, at origin
        fl::EntityTransform te{};
        te.pos[0] = 2000.0; // escortee at standoffM distance on +X
        te.quat[3] = 1.f;
        escorteeId = em.spawn("test:basic", te); // index=1
        fl::EntityTransform tt{};
        tt.pos[0] = 400.0; // threat 400 m from escort (< standoffM*0.5=1000)
        tt.quat[3] = 1.f;
        threatId = em.spawn("test:basic", tt); // index=2
    }
};

} // namespace

// --- patrol_attack: construction and error handling ---

TEST_CASE("AiControllerFactory: patrol_attack creates controller") {
    PatrolAttackFixture f;
    std::string idxStr = std::to_string(f.targetId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("patrol_attack", std::span{args}, &f.em);
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: patrol_attack with all optional args creates controller") {
    PatrolAttackFixture f;
    std::string idxStr = std::to_string(f.targetId.index);
    std::vector<std::string_view> args = {idxStr, "5000", "0.3"};
    auto ctrl = fl::ai::createController("patrol_attack", std::span{args}, &f.em);
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: patrol_attack missing entityIdx returns nullptr") {
    PatrolAttackFixture f;
    std::vector<std::string_view> args;
    CHECK(fl::ai::createController("patrol_attack", std::span{args}, &f.em) == nullptr);
}

TEST_CASE("AiControllerFactory: patrol_attack null entityManager returns nullptr") {
    std::vector<std::string_view> args = {"0"};
    CHECK(fl::ai::createController("patrol_attack", std::span{args}, nullptr) == nullptr);
}

TEST_CASE("AiControllerFactory: patrol_attack nonexistent entity returns nullptr") {
    PatrolAttackFixture f;
    std::vector<std::string_view> args = {"9999"};
    CHECK(fl::ai::createController("patrol_attack", std::span{args}, &f.em) == nullptr);
}

TEST_CASE("AiControllerFactory: patrol_attack invalid engageRangeM returns nullptr") {
    PatrolAttackFixture f;
    std::string idxStr = std::to_string(f.targetId.index);
    std::vector<std::string_view> args = {idxStr, "not_a_float"};
    CHECK(fl::ai::createController("patrol_attack", std::span{args}, &f.em) == nullptr);
}

// --- patrol_attack: initial state and output ---

TEST_CASE("AiControllerFactory: patrol_attack initial state is patrol") {
    PatrolAttackFixture f;
    std::string idxStr = std::to_string(f.targetId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("patrol_attack", std::span{args}, &f.em);
    REQUIRE(ctrl != nullptr);
    auto* sm = dynamic_cast<fl::ai::StateMachineController*>(ctrl.get());
    REQUIRE(sm != nullptr);
    CHECK(sm->currentState() == "patrol");
}

TEST_CASE("AiControllerFactory: patrol_attack patrol state outputs non-zero throttle") {
    PatrolAttackFixture f;
    std::string idxStr = std::to_string(f.targetId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("patrol_attack", std::span{args}, &f.em);
    REQUIRE(ctrl != nullptr);

    fl::EntityState selfState = makeState(3000.0, 600.0, 0.0); // on orbit radius
    selfState.id = f.selfId;
    fl::ControlInput out = ctrl->sample(selfState, 0, 1.0 / 60.0);
    CHECK(out.throttle > 0.f); // LoiterController always applies throttle
}

// --- patrol_attack: transitions ---

TEST_CASE("AiControllerFactory: patrol_attack transitions to engage when target in range") {
    PatrolAttackFixture f;
    std::string idxStr = std::to_string(f.targetId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("patrol_attack", std::span{args}, &f.em);
    REQUIRE(ctrl != nullptr);
    auto* sm = dynamic_cast<fl::ai::StateMachineController*>(ctrl.get());
    REQUIRE(sm != nullptr);

    // Self at origin; target at {1000,0,0}: dist ≈ 1166 m < 8000 m engage range.
    fl::EntityState selfState = makeState(0.0, 600.0, 0.0);
    selfState.id = f.selfId;
    sm->sample(selfState, 0, 1.0 / 60.0); // patrol output; ThreatWithinRange fires → engage
    CHECK(sm->currentState() == "engage");
}

TEST_CASE("AiControllerFactory: patrol_attack engage state outputs LeadPursuitController throttle") {
    PatrolAttackFixture f;
    std::string idxStr = std::to_string(f.targetId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("patrol_attack", std::span{args}, &f.em);
    REQUIRE(ctrl != nullptr);
    auto* sm = dynamic_cast<fl::ai::StateMachineController*>(ctrl.get());
    REQUIRE(sm != nullptr);

    fl::EntityState selfState = makeState(0.0, 600.0, 0.0);
    selfState.id = f.selfId;
    sm->sample(selfState, 0, 1.0 / 60.0); // → engage
    REQUIRE(sm->currentState() == "engage");

    fl::ControlInput engageOut = sm->sample(selfState, 1, 1.0 / 60.0);
    CHECK(engageOut.throttle == Catch::Approx(0.9f).margin(1e-5f)); // LeadPursuitController default
}

TEST_CASE("AiControllerFactory: patrol_attack transitions from engage to retreat on low HP") {
    PatrolAttackFixture f;
    std::string idxStr = std::to_string(f.targetId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("patrol_attack", std::span{args}, &f.em);
    REQUIRE(ctrl != nullptr);
    auto* sm = dynamic_cast<fl::ai::StateMachineController*>(ctrl.get());
    REQUIRE(sm != nullptr);

    fl::EntityState selfState = makeState(0.0, 600.0, 0.0);
    selfState.id = f.selfId;
    selfState.hp = 10.f;
    selfState.maxHp = 100.f; // 10% HP < 25% retreatHp threshold

    sm->sample(selfState, 0, 1.0 / 60.0); // patrol → engage
    REQUIRE(sm->currentState() == "engage");
    sm->sample(selfState, 1, 1.0 / 60.0); // engage; HpBelow(0.25) fires → retreat
    CHECK(sm->currentState() == "retreat");
}

TEST_CASE("AiControllerFactory: patrol_attack retreat state outputs EvadeController full throttle and afterburner") {
    PatrolAttackFixture f;
    std::string idxStr = std::to_string(f.targetId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("patrol_attack", std::span{args}, &f.em);
    REQUIRE(ctrl != nullptr);
    auto* sm = dynamic_cast<fl::ai::StateMachineController*>(ctrl.get());
    REQUIRE(sm != nullptr);

    fl::EntityState selfState = makeState(0.0, 600.0, 0.0);
    selfState.id = f.selfId;
    selfState.hp = 10.f;
    selfState.maxHp = 100.f;

    sm->sample(selfState, 0, 1.0 / 60.0); // → engage
    sm->sample(selfState, 1, 1.0 / 60.0); // → retreat
    REQUIRE(sm->currentState() == "retreat");

    fl::ControlInput retreatOut = sm->sample(selfState, 2, 1.0 / 60.0);
    CHECK(retreatOut.throttle == Catch::Approx(1.0f).margin(1e-5f)); // EvadeController full throttle
    CHECK(retreatOut.afterburner == true);                           // EvadeController useAfterburner=true
}

TEST_CASE("AiControllerFactory: patrol_attack transitions from engage to patrol when threat leaves after dwell") {
    PatrolAttackFixture f;
    std::string idxStr = std::to_string(f.targetId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("patrol_attack", std::span{args}, &f.em);
    REQUIRE(ctrl != nullptr);
    auto* sm = dynamic_cast<fl::ai::StateMachineController*>(ctrl.get());
    REQUIRE(sm != nullptr);

    fl::EntityState selfState = makeState(0.0, 600.0, 0.0);
    selfState.id = f.selfId;

    sm->sample(selfState, 0, 1.0 / 60.0); // → engage
    REQUIRE(sm->currentState() == "engage");

    // Move target beyond the 8000 * 1.5 = 12000 m disengage threshold.
    fl::EntityState* tgt = f.em.get(f.targetId);
    REQUIRE(tgt != nullptr);
    tgt->transform.pos[0] = 15000.0;

    // dt=2.1 s exceeds minDwell=2 s; ThreatBeyondRange(15000 > 12000) fires.
    sm->sample(selfState, 1, 2.1);
    CHECK(sm->currentState() == "patrol");
}

TEST_CASE("AiControllerFactory: patrol_attack dead target in engage transitions to patrol after dwell") {
    PatrolAttackFixture f;
    std::string idxStr = std::to_string(f.targetId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("patrol_attack", std::span{args}, &f.em);
    REQUIRE(ctrl != nullptr);
    auto* sm = dynamic_cast<fl::ai::StateMachineController*>(ctrl.get());
    REQUIRE(sm != nullptr);

    fl::EntityState selfState = makeState(0.0, 600.0, 0.0);
    selfState.id = f.selfId;

    sm->sample(selfState, 0, 1.0 / 60.0); // → engage
    REQUIRE(sm->currentState() == "engage");

    // Kill the target — ThreatBeyondRange returns true for dead/invalid entities.
    f.em.kill(f.targetId);
    f.em.onTick(1.0 / 60.0, 1);

    sm->sample(selfState, 1, 2.1); // dt=2.1 > minDwell=2 s; dead target = beyond range → patrol
    CHECK(sm->currentState() == "patrol");
}

// --- escort: construction and error handling ---

TEST_CASE("AiControllerFactory: escort creates controller") {
    EscortFixture f;
    std::string idxStr = std::to_string(f.escorteeId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("escort", std::span{args}, &f.em);
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: escort with custom standoffM creates controller") {
    EscortFixture f;
    std::string idxStr = std::to_string(f.escorteeId.index);
    std::vector<std::string_view> args = {idxStr, "3000"};
    auto ctrl = fl::ai::createController("escort", std::span{args}, &f.em);
    CHECK(ctrl != nullptr);
}

TEST_CASE("AiControllerFactory: escort missing entityIdx returns nullptr") {
    EscortFixture f;
    std::vector<std::string_view> args;
    CHECK(fl::ai::createController("escort", std::span{args}, &f.em) == nullptr);
}

TEST_CASE("AiControllerFactory: escort null entityManager returns nullptr") {
    std::vector<std::string_view> args = {"0"};
    CHECK(fl::ai::createController("escort", std::span{args}, nullptr) == nullptr);
}

TEST_CASE("AiControllerFactory: escort nonexistent entity returns nullptr") {
    EscortFixture f;
    std::vector<std::string_view> args = {"9999"};
    CHECK(fl::ai::createController("escort", std::span{args}, &f.em) == nullptr);
}

TEST_CASE("AiControllerFactory: escort invalid standoffM returns nullptr") {
    EscortFixture f;
    std::string idxStr = std::to_string(f.escorteeId.index);
    std::vector<std::string_view> args = {idxStr, "bad_float"};
    CHECK(fl::ai::createController("escort", std::span{args}, &f.em) == nullptr);
}

// --- escort: initial state and output ---

TEST_CASE("AiControllerFactory: escort initial state is follow") {
    EscortFixture f;
    std::string idxStr = std::to_string(f.escorteeId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("escort", std::span{args}, &f.em);
    REQUIRE(ctrl != nullptr);
    auto* sm = dynamic_cast<fl::ai::StateMachineController*>(ctrl.get());
    REQUIRE(sm != nullptr);
    CHECK(sm->currentState() == "follow");
}

TEST_CASE("AiControllerFactory: escort follow state outputs non-zero throttle") {
    EscortFixture f;
    std::string idxStr = std::to_string(f.escorteeId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("escort", std::span{args}, &f.em);
    REQUIRE(ctrl != nullptr);

    fl::EntityState selfState = makeState(0.0, 600.0, 0.0);
    selfState.id = f.selfId;
    fl::ControlInput out = ctrl->sample(selfState, 0, 1.0 / 60.0, nullptr);
    CHECK(out.throttle > 0.f); // LoiterController always applies throttle
}

// --- escort: transitions ---

TEST_CASE("AiControllerFactory: escort stays in follow with null SpatialIndex") {
    EscortFixture f;
    std::string idxStr = std::to_string(f.escorteeId.index);
    std::vector<std::string_view> args = {idxStr};
    auto ctrl = fl::ai::createController("escort", std::span{args}, &f.em);
    REQUIRE(ctrl != nullptr);
    auto* sm = dynamic_cast<fl::ai::StateMachineController*>(ctrl.get());
    REQUIRE(sm != nullptr);

    fl::EntityState selfState = makeState(0.0, 600.0, 0.0);
    selfState.id = f.selfId;
    // AnyEntityWithinRange returns false when si == nullptr; no transition fires.
    sm->sample(selfState, 0, 1.0 / 60.0, nullptr);
    CHECK(sm->currentState() == "follow");
}

TEST_CASE("AiControllerFactory: escort transitions to break when entity within inner range") {
    EscortFixture f;
    std::string idxStr = std::to_string(f.escorteeId.index);
    std::vector<std::string_view> args = {idxStr, "2000"};
    auto ctrl = fl::ai::createController("escort", std::span{args}, &f.em);
    REQUIRE(ctrl != nullptr);
    auto* sm = dynamic_cast<fl::ai::StateMachineController*>(ctrl.get());
    REQUIRE(sm != nullptr);
    CHECK(sm->currentState() == "follow");

    fl::EntityState selfState = makeState(0.0, 600.0, 0.0);
    selfState.id = f.selfId; // index=0; not self-excluded from AnyEntityWithinRange

    // Threat (index=2) at 400 m from self — within innerRange = standoffM*0.5 = 1000 m.
    fl::SpatialIndex si;
    double threatPos[3] = {400.0, 600.0, 0.0};
    si.insert(f.threatId.index, threatPos); // index=2

    sm->sample(selfState, 0, 1.0 / 60.0, &si); // AnyEntityWithinRange(1000) fires → break
    CHECK(sm->currentState() == "break");
}
