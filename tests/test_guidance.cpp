// SPDX-License-Identifier: GPL-3.0-or-later
//
// Local-level navigation guidance (#478): verifies fl::ai::horizontalHeadingError and
// pitchErrorFromAlt operate in the LOCAL tangent (ENU) frame at the entity's position, so AI
// steers correctly both near the world origin AND far from it (where world-Y is no longer "up").
#include "ILogger.h"
#include "ai/Guidance.h"
#include "ai/LoiterController.h"
#include "ai/PursuitController.h"
#include "ai/WaypointController.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "flight/Geodetic.h"
#include "flight/LocalFrame.h"
#include "mock_log.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <glm/glm.hpp>
#include <numbers>

using namespace fl;

namespace {

// World-space point on the sphere's surface at the equator (lon=0): planet centre is {0,-R,0}, so
// this sits at {0,-R,R} where the local "up" is +Z (NOT world +Y). A far-from-origin test anchor
// that exercises a tangent frame completely rotated away from the world axes.
constexpr double kR = fl::kEarthRadiusM;
constexpr double kEqX = 0.0;
constexpr double kEqY = -kR;
constexpr double kEqZ = kR;

// Quaternion [x,y,z,w] for a rotation of angleDeg about the +Z world axis.
static void zRotQuat(float angleDeg, float (&q)[4]) {
    const float h = angleDeg * 0.5f * static_cast<float>(std::numbers::pi_v<double> / 180.0);
    q[0] = 0.f;
    q[1] = 0.f;
    q[2] = std::sin(h);
    q[3] = std::cos(h);
}

static fl::EntityDef basicDef() {
    fl::EntityDef d;
    d.id = "test:basic";
    d.name = "Basic";
    d.category = fl::ObjectCategory::AirVehicle;
    d.maxHp = 100.f;
    return d;
}

} // namespace

// ---------------------------------------------------------------------------
// Local-frame basis sanity at the far anchor
// ---------------------------------------------------------------------------

TEST_CASE("Guidance far-from-origin: local up at the equator anchor is world +Z") {
    const glm::vec3 up = fl::radialUp(glm::dvec3{kEqX, kEqY, kEqZ}, kR);
    CHECK(up.x == Catch::Approx(0.f).margin(1e-4f));
    CHECK(up.y == Catch::Approx(0.f).margin(1e-4f));
    CHECK(up.z == Catch::Approx(1.f).margin(1e-4f));
}

// ---------------------------------------------------------------------------
// horizontalHeadingError in the local plane, far from origin
// ---------------------------------------------------------------------------

TEST_CASE("Guidance far-from-origin: heading error zero for target straight ahead") {
    // Face local north (+Y): body +X rotated +90 deg about +Z maps to world +Y.
    float q[4];
    zRotQuat(90.f, q);
    double own[3] = {kEqX, kEqY, kEqZ};
    double ahead[3] = {kEqX, kEqY + 5000.0, kEqZ}; // 5 km along local north
    CHECK(fl::ai::horizontalHeadingError(q, own, ahead) == Catch::Approx(0.f).margin(1e-3f));
}

TEST_CASE("Guidance far-from-origin: target to local right yields positive heading error") {
    // Facing local north (+Y). Local east is +X — that is the entity's RIGHT.
    float q[4];
    zRotQuat(90.f, q);
    double own[3] = {kEqX, kEqY, kEqZ};
    double right[3] = {kEqX + 3000.0, kEqY, kEqZ}; // local east
    double left[3] = {kEqX - 3000.0, kEqY, kEqZ};  // local west
    CHECK(fl::ai::horizontalHeadingError(q, own, right) > 0.f);
    CHECK(fl::ai::horizontalHeadingError(q, own, left) < 0.f);
}

TEST_CASE("Guidance far-from-origin: purely radial (up) offset gives no horizontal error") {
    // A target displaced only along local up (+Z here) has zero tangent-plane separation.
    float q[4];
    zRotQuat(90.f, q);
    double own[3] = {kEqX, kEqY, kEqZ};
    double above[3] = {kEqX, kEqY, kEqZ + 2000.0};
    CHECK(fl::ai::horizontalHeadingError(q, own, above) == Catch::Approx(0.f).margin(1e-3f));
}

// ---------------------------------------------------------------------------
// pitchErrorFromAlt relative to the local horizon, far from origin
// ---------------------------------------------------------------------------

TEST_CASE("Guidance far-from-origin: level attitude commands nose-up for target above") {
    // Facing local north (+Y) is level with the local horizon (perpendicular to up=+Z).
    float q[4];
    zRotQuat(90.f, q);
    double own[3] = {kEqX, kEqY, kEqZ};
    float pitchErr = fl::ai::pitchErrorFromAlt(q, own, 100.f);
    CHECK(pitchErr > 0.f); // 100 m above -> pitch up
    CHECK(fl::ai::pitchErrorFromAlt(q, own, -100.f) < 0.f);
}

TEST_CASE("Guidance far-from-origin: nose pointed radially up reads high current pitch") {
    // Body +X aligned with local up (+Z): rotation of -90 deg about +Y maps +X to +Z.
    // pitchOf must report ~+90 deg, so pitchErrorFromAlt(altErr=0) returns ~-90 deg.
    float q[4] = {0.f, std::sin(-45.f * static_cast<float>(std::numbers::pi_v<double> / 180.0)), 0.f,
                  std::cos(-45.f * static_cast<float>(std::numbers::pi_v<double> / 180.0))};
    double own[3] = {kEqX, kEqY, kEqZ};
    const float halfPi = std::numbers::pi_v<float> / 2.f;
    CHECK(fl::pitchOf(q, glm::dvec3{kEqX, kEqY, kEqZ}, kR) == Catch::Approx(halfPi).margin(1e-2f));
    CHECK(fl::ai::pitchErrorFromAlt(q, own, 0.f) == Catch::Approx(-halfPi).margin(1e-2f));
}

// ---------------------------------------------------------------------------
// Near-origin equivalence: the ENU frame ~= old world-XZ/Y frame at the origin
// ---------------------------------------------------------------------------

TEST_CASE("Guidance near-origin: right target positive, left target negative (world +Z is right)") {
    float q[4] = {0.f, 0.f, 0.f, 1.f}; // identity: forward = +X
    double own[3] = {0.0, 600.0, 0.0};
    double right[3] = {1000.0, 600.0, 1000.0}; // +Z side
    double left[3] = {1000.0, 600.0, -1000.0};
    CHECK(fl::ai::horizontalHeadingError(q, own, right) > 0.f);
    CHECK(fl::ai::horizontalHeadingError(q, own, left) < 0.f);
}

// ---------------------------------------------------------------------------
// Controllers steer correctly far from origin
// ---------------------------------------------------------------------------

TEST_CASE("PursuitController far-from-origin: banks toward a target on the local right") {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(basicDef());
    fl::EntityManager em(log, reg);

    // Attacker at the equator anchor facing local north (+Y).
    float q[4];
    zRotQuat(90.f, q);
    fl::EntityTransform ta{};
    ta.pos[0] = kEqX;
    ta.pos[1] = kEqY;
    ta.pos[2] = kEqZ;
    ta.quat[0] = q[0];
    ta.quat[1] = q[1];
    ta.quat[2] = q[2];
    ta.quat[3] = q[3];
    fl::EntityId attackerId = em.spawn("test:basic", ta);

    // Target 3 km to the local right (local east = +X here).
    fl::EntityTransform tt{};
    tt.pos[0] = kEqX + 3000.0;
    tt.pos[1] = kEqY;
    tt.pos[2] = kEqZ;
    tt.quat[3] = 1.f;
    fl::EntityId targetId = em.spawn("test:basic", tt);

    fl::ai::PursuitController ctrl(em, targetId);
    // Default planet radius is Earth (kEarthRadiusM), which matches the anchor's sphere.
    const fl::EntityState* as = em.get(attackerId);
    REQUIRE(as != nullptr);
    fl::ControlInput inp = ctrl.sample(*as, 0, 1.0 / 60.0);

    CHECK(inp.throttle > 0.f);
    CHECK(inp.aileron > 0.f); // target on the right -> bank right
}

TEST_CASE("WaypointController far-from-origin: banks toward a waypoint on the local right") {
    // Attacker at the equator anchor facing local north (+Y); waypoint to local east (+X).
    float q[4];
    zRotQuat(90.f, q);
    fl::EntityState s{};
    s.id = {1, 1};
    s.transform.pos[0] = kEqX;
    s.transform.pos[1] = kEqY;
    s.transform.pos[2] = kEqZ;
    s.transform.quat[0] = q[0];
    s.transform.quat[1] = q[1];
    s.transform.quat[2] = q[2];
    s.transform.quat[3] = q[3];

    fl::ai::WaypointController ctrl({glm::dvec3{kEqX + 5000.0, kEqY, kEqZ}});
    fl::ControlInput inp = ctrl.sample(s, 0, 1.0 / 60.0);
    CHECK(inp.throttle > 0.f);
    CHECK(inp.aileron > 0.f);
}

// ---------------------------------------------------------------------------
// The shared steering tail and pursuit offset (#1259)
// ---------------------------------------------------------------------------

TEST_CASE("pursuitOffsetPoint: the sign of the gain is lead versus lag") {
    // The whole difference between LeadPursuitController and LagPursuitController, which were the
    // same file twice. Target 2000 m ahead on +X, tracking +Z at 100 m/s; attacker at the origin.
    const double ownPos[3] = {0.0, 0.0, 0.0};
    const float ownVel[3] = {0.f, 0.f, 0.f};
    const double tgtPos[3] = {2000.0, 0.0, 0.0};
    const float tgtVel[3] = {0.f, 0.f, 100.f};

    double lead[3], lag[3], pure[3];
    fl::ai::pursuitOffsetPoint(lead, ownPos, ownVel, tgtPos, tgtVel, 1.f);
    fl::ai::pursuitOffsetPoint(lag, ownPos, ownVel, tgtPos, tgtVel, -1.f);
    fl::ai::pursuitOffsetPoint(pure, ownPos, ownVel, tgtPos, tgtVel, 0.f);

    // Pure pursuit aims exactly at the target.
    CHECK(pure[0] == 2000.0);
    CHECK(pure[2] == 0.0);

    // Lead aims where the target is going; lag aims where it has been, by the same distance.
    CHECK(lead[2] > 0.0);
    CHECK(lag[2] < 0.0);
    CHECK(lead[2] == -lag[2]);
}

TEST_CASE("pursuitOffsetPoint: a non-closing target does not throw the aim point off the planet") {
    // Closing speed is floored at 10 m/s and time-to-intercept capped at 30 s. Without both, an
    // opening or co-speed target divides toward infinity: here the closing speed is NEGATIVE.
    const double ownPos[3] = {0.0, 0.0, 0.0};
    const float ownVel[3] = {0.f, 0.f, 0.f};
    const double tgtPos[3] = {2000.0, 0.0, 0.0};
    const float tgtVel[3] = {500.f, 0.f, 100.f}; // running away faster than we close

    double aim[3];
    fl::ai::pursuitOffsetPoint(aim, ownPos, ownVel, tgtPos, tgtVel, 1.f);

    // TTC clamps to 30 s, so the offset is bounded by velocity * 30.
    CHECK(aim[0] <= 2000.0 + 500.0 * 30.0 + 1.0);
    CHECK(aim[2] <= 100.0 * 30.0 + 1.0);
    CHECK(std::isfinite(aim[0]));
    CHECK(std::isfinite(aim[2]));
}

TEST_CASE("pursuitOffsetPoint: a target on top of us is aimed at directly") {
    // Below the 0.1 m range guard the direction is meaningless and the offset would divide by ~0.
    const double ownPos[3] = {0.0, 0.0, 0.0};
    const float ownVel[3] = {0.f, 0.f, 0.f};
    const double tgtPos[3] = {0.01, 0.0, 0.0};
    const float tgtVel[3] = {0.f, 0.f, 300.f};

    double aim[3];
    fl::ai::pursuitOffsetPoint(aim, ownPos, ownVel, tgtPos, tgtVel, 1.f);
    CHECK(aim[0] == 0.01);
    CHECK(aim[2] == 0.0);
}

TEST_CASE("steerTowardPoint: banks toward the target and respects the caller's bank limit") {
    // Attacker at the origin facing +X; target off to the right (+Z) and above.
    fl::EntityState s{};
    s.id = {1, 1};
    s.transform.quat[3] = 1.f; // identity
    const double tgt[3] = {2000.0, 500.0, 2000.0};

    fl::ControlInput tight{}, loose{};
    fl::ai::steerTowardPoint(tight, s.transform.quat, s.transform.pos, s.transform.vel, tgt, fl::kEarthRadiusM,
                             fl::ai::kApproachBankRad);
    fl::ai::steerTowardPoint(loose, s.transform.quat, s.transform.pos, s.transform.vel, tgt, fl::kEarthRadiusM,
                             fl::ai::kCombatBankRad);

    CHECK(tight.aileron > 0.f); // roll right, toward the target
    CHECK(loose.aileron > 0.f);
    // A tighter bank limit asks for less roll from the same heading error -- the #1143 property
    // that stopped these controllers winding themselves inverted.
    CHECK(tight.aileron <= loose.aileron);

    // Throttle is deliberately not the tail's business: callers own it.
    CHECK(tight.throttle == 0.f);
}
