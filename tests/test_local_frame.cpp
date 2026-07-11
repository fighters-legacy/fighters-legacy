// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the local-level frame utilities (#470).

#include "flight/LocalFrame.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cmath>
#include <numbers>

using namespace fl;

namespace {

constexpr double R = kEarthRadiusM;

// EntityTransform-order quat [x,y,z,w] that rotates the +X body axis to `forwardWorld`
// via a rotation about world +Y (sufficient for the pitch cases, which live in the X-Z plane).
std::array<float, 4> quatPitchAboutY(float theta) {
    glm::quat q = glm::angleAxis(theta, glm::vec3(0.f, 1.f, 0.f));
    return {q.x, q.y, q.z, q.w};
}

} // namespace

TEST_CASE("LocalFrame: at the world origin (north pole) up is +Y and the basis is axis-aligned") {
    const glm::dvec3 origin{0.0, 0.0, 0.0};
    const glm::vec3 up = radialUp(origin, R);
    CHECK(up.x == Catch::Approx(0.f).margin(1e-6f));
    CHECK(up.y == Catch::Approx(1.f).margin(1e-6f));
    CHECK(up.z == Catch::Approx(0.f).margin(1e-6f));

    const glm::mat3 enu = enuBasis(origin, R);
    // Deterministic pole fallback: East=+X, North=-Z, Up=+Y (right-handed).
    CHECK(enu[0] == glm::vec3(1.f, 0.f, 0.f));  // East
    CHECK(enu[1] == glm::vec3(0.f, 0.f, -1.f)); // North
    CHECK(enu[2] == glm::vec3(0.f, 1.f, 0.f));  // Up
    // Right-handed: East x North = Up.
    CHECK(glm::length(glm::cross(enu[0], enu[1]) - enu[2]) < 1e-6f);
}

TEST_CASE("LocalFrame: far from the origin, up is no longer +Y and the basis stays orthonormal") {
    // Equator at lon 0 (Geodetic.h): world (0, -R, R); radial up is +Z.
    const glm::dvec3 p{0.0, -R, R};
    const glm::vec3 up = radialUp(p, R);
    CHECK(up.z == Catch::Approx(1.f).margin(1e-9f));
    CHECK(std::abs(up.y - 1.f) > 0.5f); // definitely not world-Y up

    const glm::mat3 enu = enuBasis(p, R);
    const glm::vec3 E = enu[0], N = enu[1], U = enu[2];
    CHECK(glm::length(E) == Catch::Approx(1.f).margin(1e-6f));
    CHECK(glm::length(N) == Catch::Approx(1.f).margin(1e-6f));
    CHECK(glm::length(U) == Catch::Approx(1.f).margin(1e-6f));
    CHECK(glm::dot(E, N) == Catch::Approx(0.f).margin(1e-6f));
    CHECK(glm::dot(E, U) == Catch::Approx(0.f).margin(1e-6f));
    CHECK(glm::dot(N, U) == Catch::Approx(0.f).margin(1e-6f));
    CHECK(glm::length(glm::cross(E, N) - U) < 1e-6f); // right-handed
    CHECK(glm::length(U - up) < 1e-6f);               // Up column == radialUp
}

TEST_CASE("LocalFrame: localAltitude is the radial height above the sphere") {
    const glm::dvec3 origin{0.0, 0.0, 0.0};
    CHECK(localAltitude(origin, R) == Catch::Approx(0.0).margin(1e-6));

    // 10 km straight up (radial) from the equator point.
    const glm::dvec3 p{0.0, -R, R + 10000.0};
    CHECK(localAltitude(p, R) == Catch::Approx(10000.0).margin(1e-3));
}

TEST_CASE("LocalFrame: headingTo gives compass bearings in the local tangent plane") {
    const glm::dvec3 p{0.0, -R, R}; // equator lon 0; ENU = (+X, +Y, +Z)
    const glm::mat3 enu = enuBasis(p, R);
    const glm::dvec3 E = glm::dvec3(enu[0]);
    const glm::dvec3 N = glm::dvec3(enu[1]);

    CHECK(headingTo(p, p + N * 1000.0, R) == Catch::Approx(0.0).margin(1e-4));                        // north
    CHECK(headingTo(p, p + E * 1000.0, R) == Catch::Approx(std::numbers::pi / 2.0).margin(1e-4));     // east
    CHECK(headingTo(p, p - E * 1000.0, R) == Catch::Approx(-std::numbers::pi / 2.0).margin(1e-4));    // west
    CHECK(std::abs(headingTo(p, p - N * 1000.0, R)) == Catch::Approx(std::numbers::pi).margin(1e-4)); // south
    // Northeast -> ~45 deg.
    CHECK(headingTo(p, p + (N + E) * 1000.0, R) == Catch::Approx(std::numbers::pi / 4.0).margin(1e-4));
}

TEST_CASE("LocalFrame: pitchOf is measured against the local horizon") {
    const glm::dvec3 p{0.0, -R, R}; // up is +Z here
    // forward = +X (east, horizontal) -> pitch 0.
    auto qLevel = quatPitchAboutY(0.f);
    CHECK(pitchOf(qLevel.data(), p, R) == Catch::Approx(0.f).margin(1e-5f));
    // Rotate +X toward +Z (up) by -90 deg about Y -> nose straight up, pitch +90.
    auto qUp = quatPitchAboutY(-static_cast<float>(std::numbers::pi) / 2.f);
    CHECK(pitchOf(qUp.data(), p, R) == Catch::Approx(static_cast<float>(std::numbers::pi) / 2.f).margin(1e-4f));
    // +90 about Y -> nose straight down, pitch -90.
    auto qDown = quatPitchAboutY(static_cast<float>(std::numbers::pi) / 2.f);
    CHECK(pitchOf(qDown.data(), p, R) == Catch::Approx(-static_cast<float>(std::numbers::pi) / 2.f).margin(1e-4f));
    // -30 about Y -> nose 30 deg above the horizon.
    auto q30 = quatPitchAboutY(-static_cast<float>(std::numbers::pi) / 6.f);
    CHECK(pitchOf(q30.data(), p, R) == Catch::Approx(static_cast<float>(std::numbers::pi) / 6.f).margin(1e-4f));
}

namespace {
// Quaternion [x,y,z,w] rotating `angle` (rad) about an arbitrary world axis.
std::array<float, 4> quatAxisAngle(glm::vec3 axis, float angle) {
    glm::quat q = glm::angleAxis(angle, glm::normalize(axis));
    return {q.x, q.y, q.z, q.w};
}
} // namespace

TEST_CASE("LocalFrame: headingOf is the nose compass bearing in the local tangent plane") {
    const glm::dvec3 p{0.0, -R, R}; // equator lon 0: ENU = East +X, North +Y, Up +Z
    const float halfPi = static_cast<float>(std::numbers::pi) / 2.f;

    // forward = +X (East): identity quat -> heading +90 deg.
    const std::array<float, 4> qEast{0.f, 0.f, 0.f, 1.f};
    CHECK(headingOf(qEast.data(), p, R) == Catch::Approx(halfPi).margin(1e-4f));
    // forward = +Y (North): rotate +X to +Y about +Z -> heading 0.
    auto qNorth = quatAxisAngle({0.f, 0.f, 1.f}, halfPi);
    CHECK(headingOf(qNorth.data(), p, R) == Catch::Approx(0.f).margin(1e-4f));
    // forward = -X (West): rotate +X to -X about +Z -> heading -90 deg.
    auto qWest = quatAxisAngle({0.f, 0.f, 1.f}, static_cast<float>(std::numbers::pi));
    CHECK(headingOf(qWest.data(), p, R) == Catch::Approx(-halfPi).margin(1e-4f));
}

TEST_CASE("LocalFrame: bankOf is roll relative to the local up") {
    const float sixth = static_cast<float>(std::numbers::pi) / 6.f;

    SECTION("at the origin, level flight is zero bank and a right roll is positive") {
        const glm::dvec3 origin{0.0, 0.0, 0.0}; // up = +Y
        const std::array<float, 4> qLevel{0.f, 0.f, 0.f, 1.f};
        CHECK(bankOf(qLevel.data(), origin, R) == Catch::Approx(0.f).margin(1e-5f));
        // Roll +30 deg about the forward (+X) axis -> +30 deg bank (matches atan2(-right.y, up.y)).
        auto qRoll = quatAxisAngle({1.f, 0.f, 0.f}, sixth);
        CHECK(bankOf(qRoll.data(), origin, R) == Catch::Approx(sixth).margin(1e-4f));
    }

    SECTION("far from the origin, a wings-level attitude on the local up reads zero bank") {
        const glm::dvec3 p{0.0, -R, R}; // up = +Z here
        // Body up -> +Z (local up), forward stays +X: rotate +90 deg about +X.
        auto qLevel = quatAxisAngle({1.f, 0.f, 0.f}, static_cast<float>(std::numbers::pi) / 2.f);
        CHECK(bankOf(qLevel.data(), p, R) == Catch::Approx(0.f).margin(1e-4f));
    }
}
