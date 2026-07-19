// SPDX-License-Identifier: GPL-3.0-or-later
//
// Client turret pose predictor (#966/#979): predicts the gunner's turret locally with the SAME pure
// stepTurret the server runs, and reconciles to the replicated pose without snapping under normal RTT.

#include "TurretPredictor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace fl;
using Catch::Approx;

namespace {
TurretLimits fullRing() {
    TurretLimits l;
    l.azMinRad = -3.14159265f;
    l.azMaxRad = 3.14159265f;
    l.elMinRad = -1.4f;
    l.elMaxRad = 1.4f;
    l.slewRateRadS = 1.5708f; // 90 deg/s
    return l;
}
} // namespace

TEST_CASE("TurretPredictor: predict slews toward the commanded aim at the servo rate (#979)", "[turret][predict]") {
    TurretPredictor p;
    p.configure(fullRing());
    p.seed(0.f, 0.f);

    // Command +1.0 rad azimuth; at 90 deg/s (~1.571 rad/s) one 0.5 s step covers ~0.785 rad, not the
    // full rad — the local response is immediate but rate-limited exactly like the server.
    p.predict(1.0f, 0.f, 0.5f);
    CHECK(p.azRad() > 0.f);
    CHECK(p.azRad() < 1.0f);
    CHECK(p.azRad() == Approx(0.7854f).margin(0.02));

    // Keep commanding; it reaches the target and holds (no overshoot).
    for (int i = 0; i < 10; ++i)
        p.predict(1.0f, 0.f, 0.1f);
    CHECK(p.azRad() == Approx(1.0f).margin(1e-3));
}

TEST_CASE("TurretPredictor: reconcile blends toward the server pose; a large gap snaps (#979)", "[turret][predict]") {
    TurretPredictor p;
    p.configure(fullRing());

    // First reconcile before any seed just adopts the server pose.
    p.reconcile(0.3f, -0.1f);
    CHECK(p.azRad() == Approx(0.3f));
    CHECK(p.elRad() == Approx(-0.1f));

    // A small divergence blends (does not snap).
    p.seed(0.30f, 0.f);
    p.reconcile(0.34f, 0.f, /*blend=*/0.25f);
    CHECK(p.azRad() == Approx(0.30f + 0.04f * 0.25f).margin(1e-4)); // moved 25% of the 0.04 gap

    // A large divergence (> snapRad) snaps to the server pose (no long visible drift).
    p.seed(0.0f, 0.f);
    p.reconcile(1.0f, 0.f, /*blend=*/0.2f, /*snapRad=*/0.35f);
    CHECK(p.azRad() == Approx(1.0f)); // snapped
}

TEST_CASE("TurretPredictor: prediction converges to the server pose over a few reconciles (#979)",
          "[turret][predict]") {
    // Model: the client predicts ahead of a lagging server pose; each frame it reconciles a fraction
    // toward the (advancing) server truth. It must converge smoothly, never snapping.
    TurretPredictor p;
    p.configure(fullRing());
    p.seed(0.f, 0.f);

    const float serverTarget = 0.5f;
    for (int i = 0; i < 60; ++i) {
        p.predict(serverTarget, 0.f, 1.0f / 60.f); // local response toward the commanded aim
        p.reconcile(serverTarget, 0.f, 0.1f);      // pull toward the (settled) server pose
    }
    CHECK(p.azRad() == Approx(serverTarget).margin(0.01)); // converged, no rubber-banding
}

TEST_CASE("TurretPredictor: predictWorld decomposes a world aim into the mount frame (#979)", "[turret][predict]") {
    TurretPredictor p;
    p.configure(fullRing());
    p.seed(0.f, 0.f);

    // Identity mount + airframe: aiming straight up (+Y) should drive elevation to +pi/2 (clamped by the
    // limit), azimuth ~0.
    const glm::quat ident{1.f, 0.f, 0.f, 0.f};
    for (int i = 0; i < 120; ++i)
        p.predictWorld(glm::vec3{0.f, 1.f, 0.f}, ident, ident, 1.0f / 60.f);
    CHECK(p.elRad() == Approx(1.4f).margin(0.02)); // reached the elevation limit
    CHECK(std::abs(p.azRad()) < 0.05f);
}
