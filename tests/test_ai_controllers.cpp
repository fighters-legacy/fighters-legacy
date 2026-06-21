// SPDX-License-Identifier: GPL-3.0-or-later
#include "ILogger.h"
#include "ai/AiControllerFactory.h"
#include "ai/BreakTurnController.h"
#include "ai/EvadeController.h"
#include "ai/Guidance.h"
#include "ai/LoiterController.h"
#include "ai/PursuitController.h"
#include "ai/WaypointController.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <glm/glm.hpp>
#include <numbers>
#include <string>
#include <vector>

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
