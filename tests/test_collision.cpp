// SPDX-License-Identifier: GPL-3.0-or-later
//
// Entity-entity collision geometry + damage (#630). The pure narrow-phase helpers; the parallel
// detect/serial-apply pass and its serial-equivalence live in test_world_broadcaster.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "entity/Collision.h"
#include "entity/EntityDef.h"

using namespace fl;

TEST_CASE("spheresOverlap: touching, overlapping, and clear", "[collision]") {
    const double a[3] = {0, 0, 0};
    const double b[3] = {10, 0, 0};
    CHECK(spheresOverlap(a, 6.f, b, 5.f));       // radii sum 11 > distance 10
    CHECK(spheresOverlap(a, 5.f, b, 5.f));       // exactly touching (sum 10 == distance 10)
    CHECK_FALSE(spheresOverlap(a, 4.f, b, 5.f)); // sum 9 < distance 10
}

TEST_CASE("spheresOverlap: exact far from the world origin (double precision)", "[collision]") {
    // A metre-scale overlap at planet scale must still resolve — float would lose it.
    const double a[3] = {6.37e6, 0.0, 0.0};
    const double b[3] = {6.37e6 + 12.0, 0.0, 0.0};
    CHECK(spheresOverlap(a, 8.f, b, 8.f)); // sum 16 > 12
    const double c[3] = {6.37e6 + 20.0, 0.0, 0.0};
    CHECK_FALSE(spheresOverlap(a, 8.f, c, 8.f)); // sum 16 < 20
}

TEST_CASE("relativeSpeedMps: closing and co-moving", "[collision]") {
    const float head_on_a[3] = {300.f, 0.f, 0.f};
    const float head_on_b[3] = {-300.f, 0.f, 0.f};
    CHECK(relativeSpeedMps(head_on_a, head_on_b) == Catch::Approx(600.f));

    const float same[3] = {200.f, 10.f, -5.f};
    CHECK(relativeSpeedMps(same, same) == Catch::Approx(0.f)); // a formation join-up brushing
}

TEST_CASE("collisionDamage: free below the brush threshold, then linear", "[collision]") {
    CHECK(collisionDamage(2.f) == 0.f);                    // a gentle bump
    CHECK(collisionDamage(kCollisionFreeSpeedMps) == 0.f); // exactly the threshold
    CHECK(collisionDamage(605.f) > 100.f);                 // a head-on merge kills a 100 hp airframe
    // Linear above the threshold.
    const float d1 = collisionDamage(50.f + kCollisionFreeSpeedMps);
    const float d2 = collisionDamage(100.f + kCollisionFreeSpeedMps);
    CHECK(d2 == Catch::Approx(2.f * d1));
}

TEST_CASE("defaultCollisionRadiusM: category defaults, projectiles excluded", "[collision]") {
    CHECK(defaultCollisionRadiusM(ObjectCategory::AirVehicle) == 8.f);
    CHECK(defaultCollisionRadiusM(ObjectCategory::Player) == 8.f);
    CHECK(defaultCollisionRadiusM(ObjectCategory::GroundVehicle) == 15.f);
    CHECK(defaultCollisionRadiusM(ObjectCategory::NavalVehicle) == 15.f);
    CHECK(defaultCollisionRadiusM(ObjectCategory::Structure) == 15.f);
    CHECK(defaultCollisionRadiusM(ObjectCategory::Projectile) == 0.f); // fuze path, not this phase
    CHECK(defaultCollisionRadiusM(ObjectCategory::Effect) == 0.f);
}
