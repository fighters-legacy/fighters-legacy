// SPDX-License-Identifier: GPL-3.0-or-later
//
// Head-tracking tests (#927): the pure opentrack datagram parse and the EMA/freshness filter. No
// socket — the receive path is exercised manually via HeadPoseFilter.

#include "HeadTracker.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

using namespace fl;

TEST_CASE("parseOpentrackDatagram: accepts exactly 48 bytes, rejects others (#927)", "[headtrack]") {
    const double pose[6] = {1.0, 2.0, 3.0, 10.0, 20.0, 30.0};
    auto ok = parseOpentrackDatagram(pose, sizeof(pose));
    REQUIRE(ok.has_value());
    CHECK(ok->xCm == Catch::Approx(1.0));
    CHECK(ok->yCm == Catch::Approx(2.0));
    CHECK(ok->zCm == Catch::Approx(3.0));
    CHECK(ok->yawDeg == Catch::Approx(10.0));
    CHECK(ok->pitchDeg == Catch::Approx(20.0));
    CHECK(ok->rollDeg == Catch::Approx(30.0));

    CHECK_FALSE(parseOpentrackDatagram(pose, 0).has_value());
    CHECK_FALSE(parseOpentrackDatagram(pose, 47).has_value());
    CHECK_FALSE(parseOpentrackDatagram(pose, 49).has_value());
}

TEST_CASE("HeadPoseFilter: maps degrees->radians, cm->m, and honours scale/invert (#927)", "[headtrack]") {
    HeadTrackingSettings cfg;
    cfg.smoothing = 0.0f; // raw, so the first frame passes straight through
    RawHeadPose raw{10.0, 20.0, 30.0, 90.0, -45.0, 30.0};

    HeadPoseFilter f;
    f.update(&raw, 1.0f / 60.0f, cfg);
    CHECK(f.pose.fresh);
    CHECK(f.pose.yawRad == Catch::Approx(3.14159265f / 2.0f).margin(1e-4)); // 90 deg
    CHECK(f.pose.pitchRad == Catch::Approx(-3.14159265f / 4.0f).margin(1e-4));
    // offset = (-zCm, yCm, xCm) * 0.01 -> (-0.30, 0.20, 0.10) m
    CHECK(f.pose.offsetM.x == Catch::Approx(-0.30f).margin(1e-4));
    CHECK(f.pose.offsetM.y == Catch::Approx(0.20f).margin(1e-4));
    CHECK(f.pose.offsetM.z == Catch::Approx(0.10f).margin(1e-4));

    // Invert yaw flips the sign.
    cfg.invertYaw = true;
    HeadPoseFilter f2;
    f2.update(&raw, 1.0f / 60.0f, cfg);
    CHECK(f2.pose.yawRad < 0.0f);
}

TEST_CASE("HeadPoseFilter: positional offset is clamped to +/-0.5 m (#927)", "[headtrack]") {
    HeadTrackingSettings cfg;
    cfg.smoothing = 0.0f;
    RawHeadPose raw{200.0, 200.0, 200.0, 0.0, 0.0, 0.0}; // 2 m each -> clamp
    HeadPoseFilter f;
    f.update(&raw, 1.0f / 60.0f, cfg);
    CHECK(std::abs(f.pose.offsetM.x) <= 0.5f + 1e-5f);
    CHECK(std::abs(f.pose.offsetM.y) <= 0.5f + 1e-5f);
    CHECK(std::abs(f.pose.offsetM.z) <= 0.5f + 1e-5f);
}

TEST_CASE("HeadPoseFilter: EMA converges monotonically toward a step input (#927)", "[headtrack]") {
    HeadTrackingSettings cfg;
    cfg.smoothing = 0.6f;
    RawHeadPose raw{0.0, 0.0, 0.0, 60.0, 0.0, 0.0};
    HeadPoseFilter f;
    float prev = 0.0f;
    for (int i = 0; i < 30; ++i) {
        f.update(&raw, 1.0f / 60.0f, cfg);
        CHECK(f.pose.yawRad >= prev - 1e-6f); // non-decreasing toward the target
        prev = f.pose.yawRad;
    }
    CHECK(f.pose.yawRad == Catch::Approx(3.14159265f / 3.0f).margin(0.02)); // ~60 deg after settling
}

TEST_CASE("HeadPoseFilter: freshness expires without packets and restores on a new one (#927)", "[headtrack]") {
    HeadTrackingSettings cfg;
    RawHeadPose raw{0.0, 0.0, 0.0, 10.0, 0.0, 0.0};
    HeadPoseFilter f;
    f.update(&raw, 1.0f / 60.0f, cfg);
    CHECK(f.pose.fresh);

    // 0.6 s with no packets (> 0.5 s timeout) -> stale.
    for (int i = 0; i < 40; ++i)
        f.update(nullptr, 1.0f / 60.0f, cfg);
    CHECK_FALSE(f.pose.fresh);

    // A new packet restores freshness.
    f.update(&raw, 1.0f / 60.0f, cfg);
    CHECK(f.pose.fresh);
}
