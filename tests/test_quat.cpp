// SPDX-License-Identifier: GPL-3.0-or-later
//
// fl::quatRotate / quatRotateD / quatRotateConjD / quatToEuler / quatNorm (#1248).
//
// Three independent bodies existed: FlightIntegrator's, WorldBroadcaster's, and a third inlined in
// DeckDef::deckLocalPoint. They computed the same rotation and were not interchangeable -- the
// terms were summed in different associations, so results could differ in the last ulp, which on
// the wire is one quantisation bucket.
//
// The bodies here are FlightIntegrator's, so the flight model the determinism gate pins is
// unchanged. What these tests guard is the part a reviewer cannot check by eye: that the deck
// path's conjugate rotation is BIT-IDENTICAL to the expression it replaced, and that the rotation
// obeys the properties a rotation has to obey.

#include "math/Quat.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <numbers>

using Catch::Matchers::WithinAbs;

namespace {

// A unit quaternion of `angle` about `axis`, in the [x, y, z, w] order the engine uses. The axis is
// normalised here rather than trusted: a caller-supplied axis that is a fraction off unit produces a
// non-unit quaternion, which does not preserve length -- and then the test measures the caller's
// arithmetic instead of the rotation's.
std::array<float, 4> axisAngle(float ax, float ay, float az, float angle) {
    const float n = std::sqrt(ax * ax + ay * ay + az * az);
    ax /= n;
    ay /= n;
    az /= n;
    const float s = std::sin(angle * 0.5f);
    return {ax * s, ay * s, az * s, std::cos(angle * 0.5f)};
}

} // namespace

TEST_CASE("the identity quaternion leaves a vector alone", "[quat]") {
    const float q[4] = {0.f, 0.f, 0.f, 1.f};
    const float v[3] = {1.f, -2.f, 3.5f};
    const auto r = fl::quatRotate(q, v);
    CHECK(r[0] == 1.f);
    CHECK(r[1] == -2.f);
    CHECK(r[2] == 3.5f);
}

TEST_CASE("a quarter turn about Y takes +X to -Z", "[quat]") {
    const auto q = axisAngle(0.f, 1.f, 0.f, std::numbers::pi_v<float> / 2.f);
    const float v[3] = {1.f, 0.f, 0.f};
    const auto r = fl::quatRotate(q.data(), v);
    CHECK_THAT(r[0], WithinAbs(0.f, 1e-6f));
    CHECK_THAT(r[1], WithinAbs(0.f, 1e-6f));
    CHECK_THAT(r[2], WithinAbs(-1.f, 1e-6f));
}

TEST_CASE("rotation preserves length", "[quat]") {
    const auto q = axisAngle(1.f, 2.f, 3.f, 1.1f); // an arbitrary axis
    const float v[3] = {3.f, -4.f, 12.f};          // length 13
    const auto r = fl::quatRotate(q.data(), v);
    CHECK_THAT(std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]), WithinAbs(13.f, 1e-4f));
}

TEST_CASE("quatRotateConjD is bit-identical to the expression it replaced", "[quat]") {
    // This is the guard on the "moved verbatim" claim for the deck-footprint transform. The body
    // below is DeckDef::deckLocalPoint's, exactly as it was before #1248: note it kept qw as a
    // FLOAT and cast the other components at each use. If a future edit to quatRotateConjD reorders
    // or reassociates anything, this fails rather than silently moving where the deck edge is.
    //
    // This target is built with -ffp-contract=off, and it has to be. With contraction ON, the
    // compiler may fuse a multiply-add here and not there -- the reference below converts a FLOAT
    // qw where the shared function has already widened it to double, which is enough to change
    // which products get fused. Apple Clang at -O2 does exactly that and the two sides land ~2 ulp
    // apart, which is how this test first failed on macOS while passing everywhere else.
    //
    // Contraction is a rounding difference, not a reassociation, and REASSOCIATION is what this
    // test exists to catch -- that was the actual defect between FlightIntegrator and
    // WorldBroadcaster. Turning contraction off measures the algebra instead of the code generator.
    const float shipQuat[4] = {0.183f, -0.365f, 0.548f, 0.730f};
    const double d[3] = {12.25, -3.5, 88.125};

    const float qx = -shipQuat[0], qy = -shipQuat[1], qz = -shipQuat[2], qw = shipQuat[3];
    const double tx = 2.0 * (double(qy) * d[2] - double(qz) * d[1]);
    const double ty = 2.0 * (double(qz) * d[0] - double(qx) * d[2]);
    const double tz = 2.0 * (double(qx) * d[1] - double(qy) * d[0]);
    const double ex = d[0] + qw * tx + double(qy) * tz - double(qz) * ty;
    const double ey = d[1] + qw * ty + double(qz) * tx - double(qx) * tz;
    const double ez = d[2] + qw * tz + double(qx) * ty - double(qy) * tx;

    const auto got = fl::quatRotateConjD(shipQuat, d);
    CHECK(got[0] == ex);
    CHECK(got[1] == ey);
    CHECK(got[2] == ez);
}

TEST_CASE("the conjugate rotation undoes the forward one", "[quat]") {
    const auto q = axisAngle(0.f, 0.f, 1.f, 0.7f);
    const double v[3] = {5.0, -2.0, 9.0};
    const auto world = fl::quatRotateD(q.data(), v);
    const auto back = fl::quatRotateConjD(q.data(), world.data());
    // The tolerance is set by the quaternion being FLOAT: the rotation runs in double, but its
    // coefficients carry only float precision, so the round trip closes to about 1e-7 relative.
    CHECK_THAT(back[0], WithinAbs(5.0, 1e-6));
    CHECK_THAT(back[1], WithinAbs(-2.0, 1e-6));
    CHECK_THAT(back[2], WithinAbs(9.0, 1e-6));
}

TEST_CASE("quatToEuler saturates at gimbal lock instead of leaving asin's domain", "[quat]") {
    const float kHalfPi = std::numbers::pi_v<float> / 2.f;

    // A quaternion whose sinp lands ABOVE 1 takes the copysign branch. That is the branch that
    // matters: asin outside [-1, 1] is NaN, and a NaN pitch propagates into the HUD and the AI.
    const float over[4] = {0.f, 1.f, 0.f, 1.f}; // deliberately not normalised: sinp = 2
    const auto saturated = fl::quatToEuler(over);
    CHECK(saturated[1] == kHalfPi);

    // A genuine quarter-turn about Y sits just BELOW the branch, and there asin is numerically
    // sensitive rather than wrong: sinp comes out about 6e-8 short of 1, and asin's derivative is
    // unbounded at 1, so the pitch lands ~3e-4 low. Worth pinning so nobody later "fixes" it with a
    // tighter tolerance and an epsilon nudge.
    const auto q = axisAngle(0.f, 1.f, 0.f, kHalfPi);
    const auto e = fl::quatToEuler(q.data());
    CHECK(std::isfinite(e[0]));
    CHECK(e[1] <= kHalfPi);
    CHECK_THAT(e[1], WithinAbs(kHalfPi, 1e-3f));
    CHECK(std::isfinite(e[2]));
}

TEST_CASE("quatNorm normalises, and leaves a degenerate quaternion alone", "[quat]") {
    float q[4] = {0.f, 0.f, 0.f, 4.f};
    fl::quatNorm(q);
    CHECK_THAT(q[3], WithinAbs(1.f, 1e-6f));

    // Below the epsilon it does nothing. Dividing by ~0 would turn a bug upstream into a NaN that
    // poisons every value downstream and loses the evidence of where it started.
    float zero[4] = {0.f, 0.f, 0.f, 0.f};
    fl::quatNorm(zero);
    CHECK(zero[3] == 0.f);
    CHECK(std::isfinite(zero[0]));
}
