// SPDX-License-Identifier: GPL-3.0-or-later
#include "mission/ShotDirector.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/quaternion.hpp>

#include <unordered_map>

using namespace fl;
using Catch::Approx;

namespace {

// A scripted pose source: id -> (pos, orient). Missing ids resolve false (dead/unspawned).
struct PoseTable {
    std::unordered_map<std::string, std::pair<glm::dvec3, glm::dquat>> poses;
    EntityPoseFn fn() {
        return [this](std::string_view id, glm::dvec3& pos, glm::dquat& orient) -> bool {
            auto it = poses.find(std::string(id));
            if (it == poses.end())
                return false;
            pos = it->second.first;
            orient = it->second.second;
            return true;
        };
    }
};

MissionShot staticShot(double start, double dur, glm::dvec3 pos, const std::string& lookId) {
    MissionShot s;
    s.type = ShotType::Static;
    s.startSec = start;
    s.durationSec = dur;
    s.pos[0] = pos.x;
    s.pos[1] = pos.y;
    s.pos[2] = pos.z;
    s.lookAtId = lookId;
    return s;
}

} // namespace

TEST_CASE("ShotDirector empty is safe", "[shot-director]") {
    ShotDirector d({});
    PoseTable t;
    auto p = d.evaluate(0.0, t.fn());
    CHECK(d.totalDurationSec() == 0.0);
    CHECK(p.shotIndex == -1);
    CHECK_FALSE(p.active);
}

TEST_CASE("ShotDirector static looks at a live entity", "[shot-director]") {
    PoseTable t;
    t.poses["a"] = {glm::dvec3(0, 0, 0), glm::dquat(1, 0, 0, 0)};
    ShotDirector d({staticShot(0, 10, glm::dvec3(0, 0, 100), "a")});
    auto p = d.evaluate(1.0, t.fn());
    CHECK(p.active);
    CHECK(p.shotIndex == 0);
    CHECK(p.eye.z == Approx(100.0));
    // look at origin from (0,0,100) → forward points toward -Z.
    CHECK(p.fwd.z == Approx(-1.0));
}

TEST_CASE("ShotDirector fov default and clamp", "[shot-director]") {
    PoseTable t;
    t.poses["a"] = {glm::dvec3(0, 0, 0), glm::dquat(1, 0, 0, 0)};
    auto s = staticShot(0, 10, glm::dvec3(0, 0, 100), "a");
    s.fovYDeg = 200.f; // out of range → clamp to 120 at evaluate
    ShotDirector d({s});
    CHECK(d.evaluate(1.0, t.fn()).fovYDeg == Approx(120.f));
}

TEST_CASE("ShotDirector orbit geometry, period and CW/CCW", "[shot-director]") {
    PoseTable t;
    t.poses["tgt"] = {glm::dvec3(0, 0, 0), glm::dquat(1, 0, 0, 0)};
    MissionShot s;
    s.type = ShotType::Orbit;
    s.startSec = 0;
    s.durationSec = 60;
    s.targetId = "tgt";
    s.orbitRadiusM = 400;
    s.orbitHeightM = 60;
    s.orbitPeriodSec = 40; // CCW
    ShotDirector d({s});
    auto e = t.fn();

    // t=0: angle 0 → (+radius, height, 0).
    auto p0 = d.evaluate(0.0, e);
    CHECK(p0.eye.x == Approx(400.0));
    CHECK(p0.eye.y == Approx(60.0));
    CHECK(p0.eye.z == Approx(0.0).margin(1e-9));
    // quarter period (10 s of 40) → angle 90° → (0, height, +radius).
    auto p1 = d.evaluate(10.0, e);
    CHECK(p1.eye.x == Approx(0.0).margin(1e-6));
    CHECK(p1.eye.z == Approx(400.0));
    // radius held constant.
    const double r = std::sqrt(p1.eye.x * p1.eye.x + p1.eye.z * p1.eye.z);
    CHECK(r == Approx(400.0));
    // orbit always looks at the target (origin).
    CHECK(glm::length(p1.fwd) == Approx(1.0));

    // A negative period reverses the sweep: at t=10 the z-component flips sign.
    MissionShot cw = s;
    cw.orbitPeriodSec = -40;
    ShotDirector dcw({cw});
    auto pc = dcw.evaluate(10.0, e);
    CHECK(pc.eye.z == Approx(-400.0));
}

TEST_CASE("ShotDirector chase offset frame + stiffness convergence", "[shot-director]") {
    PoseTable t;
    // Target at (0,0,0), identity orientation (body fwd = world +X).
    t.poses["p"] = {glm::dvec3(0, 0, 0), glm::dquat(1, 0, 0, 0)};
    MissionShot s;
    s.type = ShotType::Chase;
    s.startSec = 0;
    s.durationSec = 30;
    s.targetId = "p";
    s.chaseOffset[0] = -60; // 60 m aft (behind +X forward)
    s.chaseOffset[1] = 15;  // 15 m up
    s.chaseOffset[2] = 0;
    s.chaseStiffness = 0.0; // rigid first
    ShotDirector d({s});
    auto e = t.fn();
    // Rigid: eye = target + R(identity)*(-60,15,0) = (-60,15,0).
    auto p = d.evaluate(0.0, e);
    CHECK(p.eye.x == Approx(-60.0));
    CHECK(p.eye.y == Approx(15.0));

    // With smoothing and a target that jumps forward, the eye should converge toward the new rigid
    // pose over repeated fixed-step calls (never overshoot to origin).
    MissionShot ss = s;
    ss.chaseStiffness = 4.0;
    ShotDirector d2({ss});
    // First frame snaps to rigid at the initial target.
    d2.evaluate(0.0, e);
    // Move the target far along +X; step at dt=1/30 for 2 seconds and watch the eye chase it.
    t.poses["p"] = {glm::dvec3(1000, 0, 0), glm::dquat(1, 0, 0, 0)};
    glm::dvec3 last{0};
    for (int i = 1; i <= 60; ++i)
        last = d2.evaluate(i / 30.0, e).eye;
    // New rigid eye = (1000-60, 15, 0) = (940,15,0). After 2 s at k=4 it should be close.
    CHECK(last.x == Approx(940.0).margin(1.0));
    CHECK(last.y == Approx(15.0).margin(0.1));
}

TEST_CASE("ShotDirector move endpoints pass through keyframes (linear + smooth)", "[shot-director]") {
    PoseTable t;
    t.poses["a"] = {glm::dvec3(0, 0, 0), glm::dquat(1, 0, 0, 0)};
    MissionShot s;
    s.type = ShotType::Move;
    s.startSec = 0;
    s.durationSec = 10;
    s.lookAtId = "a";
    s.ease = ShotEase::Linear;
    ShotKeyframe k0;
    k0.timeSec = 0;
    k0.pos[0] = 100;
    k0.pos[1] = 50;
    k0.pos[2] = 0;
    ShotKeyframe k1;
    k1.timeSec = 10;
    k1.pos[0] = -100;
    k1.pos[1] = 50;
    k1.pos[2] = 200;
    s.keyframes = {k0, k1};
    ShotDirector d({s});
    auto e = t.fn();
    // Endpoints.
    CHECK(d.evaluate(0.0, e).eye.x == Approx(100.0));
    CHECK(d.evaluate(10.0, e).eye.x == Approx(-100.0));
    // Midpoint (linear): halfway between the two keys.
    auto mid = d.evaluate(5.0, e);
    CHECK(mid.eye.x == Approx(0.0));
    CHECK(mid.eye.z == Approx(100.0));

    // Smooth easing still passes through the keys exactly.
    MissionShot sm = s;
    sm.ease = ShotEase::Smooth;
    ShotDirector d2({sm});
    auto e2 = t.fn();
    CHECK(d2.evaluate(0.0, e2).eye.x == Approx(100.0));
    CHECK(d2.evaluate(10.0, e2).eye.x == Approx(-100.0));
}

TEST_CASE("ShotDirector three-key Catmull-Rom passes through the middle key", "[shot-director]") {
    PoseTable t;
    t.poses["a"] = {glm::dvec3(0, 0, 0), glm::dquat(1, 0, 0, 0)};
    MissionShot s;
    s.type = ShotType::Move;
    s.startSec = 0;
    s.durationSec = 10;
    s.lookAtId = "a";
    s.ease = ShotEase::Smooth;
    auto mk = [](double time, double x) {
        ShotKeyframe k;
        k.timeSec = time;
        k.pos[0] = x;
        k.pos[1] = 100;
        k.pos[2] = 0;
        return k;
    };
    s.keyframes = {mk(0, 0), mk(5, 300), mk(10, 100)};
    ShotDirector d({s});
    auto e = t.fn();
    CHECK(d.evaluate(5.0, e).eye.x == Approx(300.0)); // passes through the interior key
}

TEST_CASE("ShotDirector cut, gap-hold, before-first, after-last", "[shot-director]") {
    PoseTable t;
    t.poses["a"] = {glm::dvec3(0, 0, 0), glm::dquat(1, 0, 0, 0)};
    // Shot 0: [0,5) at eye x=100; gap [5,10); shot 1: [10,15) at eye x=-100.
    std::vector<MissionShot> shots = {staticShot(0, 5, glm::dvec3(100, 0, 0), "a"),
                                      staticShot(10, 5, glm::dvec3(-100, 0, 0), "a")};
    ShotDirector d(std::move(shots));
    auto e = t.fn();

    CHECK(d.totalDurationSec() == Approx(15.0));

    // Before first: hold shot 0's start pose, not active, shotIndex -1.
    auto pb = d.evaluate(-1.0, e);
    CHECK(pb.eye.x == Approx(100.0));
    CHECK_FALSE(pb.active);
    CHECK(pb.shotIndex == -1);

    // Inside shot 0.
    auto p0 = d.evaluate(2.0, e);
    CHECK(p0.active);
    CHECK(p0.shotIndex == 0);
    CHECK(p0.eye.x == Approx(100.0));

    // In the gap: hold shot 0's (final) pose, not active.
    auto pg = d.evaluate(7.0, e);
    CHECK_FALSE(pg.active);
    CHECK(pg.shotIndex == -1);
    CHECK(pg.eye.x == Approx(100.0)); // held from shot 0

    // Hard cut into shot 1.
    auto p1 = d.evaluate(11.0, e);
    CHECK(p1.active);
    CHECK(p1.shotIndex == 1);
    CHECK(p1.eye.x == Approx(-100.0));

    // After last: hold shot 1's final pose.
    auto pa = d.evaluate(100.0, e);
    CHECK_FALSE(pa.active);
    CHECK(pa.shotIndex == -1);
    CHECK(pa.eye.x == Approx(-100.0));
}

TEST_CASE("ShotDirector dead target holds the last valid pose", "[shot-director]") {
    PoseTable t;
    t.poses["tgt"] = {glm::dvec3(500, 0, 0), glm::dquat(1, 0, 0, 0)};
    MissionShot s;
    s.type = ShotType::Orbit;
    s.startSec = 0;
    s.durationSec = 60;
    s.targetId = "tgt";
    s.orbitRadiusM = 100;
    s.orbitPeriodSec = 60;
    ShotDirector d({s});
    auto e = t.fn();
    auto alive = d.evaluate(0.0, e);
    CHECK(alive.eye.x == Approx(600.0)); // 500 + radius 100

    // Target dies (removed from the table): the pose is held, never snapped to origin.
    t.poses.erase("tgt");
    auto dead = d.evaluate(5.0, e);
    CHECK(dead.eye.x == Approx(600.0));
    CHECK(glm::length(dead.eye) > 1.0); // not origin
}
