// SPDX-License-Identifier: GPL-3.0-or-later
//
// Target-slaved inset camera math (#698): stand-off distance, look-at correctness, the degenerate
// ownship-on-target case, and the inset rect layout. Pure — no renderer.

#include "InsetViewMath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <glm/glm.hpp>

using namespace fl;

TEST_CASE("buildTargetInsetView: eye is one standoff behind the target toward the ownship (#698)", "[inset]") {
    const glm::dvec3 target{0, 0, 0};
    const glm::dvec3 own{1000, 0, 0}; // ownship 1 km along +X
    const CameraView cv = buildTargetInsetView(target, glm::vec3{0}, 0.f, own, glm::vec3{0, 1, 0},
                                               /*rectAspect=*/1.0f, /*standoffM=*/30.0);
    // Eye sits 30 m from the target toward the ownship: at (+30, 0, 0).
    CHECK(cv.worldOrigin.x == Catch::Approx(30.0).margin(1e-6));
    CHECK(cv.worldOrigin.y == Catch::Approx(0.0).margin(1e-6));
    CHECK(cv.worldOrigin.z == Catch::Approx(0.0).margin(1e-6));
}

TEST_CASE("buildTargetInsetView: the camera looks at the target (#698)", "[inset]") {
    const glm::dvec3 target{500, 200, -100};
    const glm::dvec3 own{500, 200, 2000};
    const CameraView cv = buildTargetInsetView(target, glm::vec3{0}, 0.f, own, glm::vec3{0, 1, 0}, 1.0f, 30.0);
    // The view matrix maps the target (in camera-relative coords) onto the -Z view axis.
    const glm::vec3 rel = glm::vec3(target - cv.worldOrigin);
    const glm::vec4 viewPos = cv.view * glm::vec4(rel, 1.0f);
    // Looking down -Z: the target's view-space z is negative and x/y ~ 0.
    CHECK(viewPos.z < 0.0f);
    CHECK(std::abs(viewPos.x) < 1e-3f);
    CHECK(std::abs(viewPos.y) < 1e-3f);
}

TEST_CASE("buildTargetInsetView: the degenerate ownship-on-target case is safe (#698)", "[inset]") {
    const glm::dvec3 p{10, 20, 30};
    const CameraView cv = buildTargetInsetView(p, glm::vec3{0}, 0.f, p, glm::vec3{0, 1, 0}, 1.0f, 30.0);
    // No NaNs, eye is a finite standoff away from the coincident point.
    CHECK(std::isfinite(cv.worldOrigin.x));
    CHECK(std::isfinite(cv.worldOrigin.y));
    CHECK(std::isfinite(cv.worldOrigin.z));
    CHECK(glm::length(cv.worldOrigin - p) == Catch::Approx(30.0).margin(1e-3));
}

TEST_CASE("insetRectFor: bottom-centre, square-pixel, on-screen (#698)", "[inset]") {
    const glm::vec4 r = insetRectFor(16.f / 9.f);
    CHECK(r.x > 0.0f);
    CHECK(r.x + r.z < 1.0f);                        // fully on-screen horizontally
    CHECK(r.y + r.w == Catch::Approx(0.97f));       // sits just above the bottom edge
    CHECK(r.x == Catch::Approx(0.5f - r.z * 0.5f)); // horizontally centred
    // Square pixels: normalized height = normalized width * aspect.
    CHECK(r.w == Catch::Approx(r.z * (16.f / 9.f)));
}

// ---------------------------------------------------------------------------
// The one sub-tick extrapolation (#1250)
//
// Six sites spelled this out: the scene renderer, four camera paths, and this inset view. They MUST
// agree -- the camera target is taken from the same snapshot as the rendered entity so the two are
// coincident, and a divergent copy detaches the camera from the thing it is looking at. That has
// happened once already: a hardcoded 1/60 here disagreed with alpha on any server not at 60 Hz.
// ---------------------------------------------------------------------------

TEST_CASE("extrapolatePosition is the expression the six sites used", "[inset][extrapolate]") {
    const glm::dvec3 pos{1000.0, 2000.0, -3000.0};
    const glm::vec3 vel{250.f, -12.f, 7.f};
    const float alpha = 0.37f;

    // Bit-identical to the body it replaced, spelled out. A refactor of the helper that reassociates
    // this would move every rendered entity a little, and the camera a different little.
    const glm::dvec3 expected = pos + glm::dvec3(vel * (alpha * kServerTickRate.dtSeconds()));
    CHECK(extrapolatePosition(pos, vel, alpha, kServerTickRate) == expected);
}

TEST_CASE("extrapolatePosition scales with alpha and the server's period", "[inset][extrapolate]") {
    const glm::dvec3 pos{0.0, 0.0, 0.0};
    const glm::vec3 vel{100.f, 0.f, 0.f};

    // alpha = 0 is the snapshot itself; alpha = 1 is one whole server tick further on.
    CHECK(extrapolatePosition(pos, vel, 0.f, kServerTickRate) == pos);
    CHECK(extrapolatePosition(pos, vel, 1.f, kServerTickRate).x == Catch::Approx(100.0 * kServerTickRate.dtSeconds()));

    // alpha is "how far through a SERVER tick", so a slower server extrapolates further for the
    // same alpha. This is the #1075 property a hardcoded 1/60 destroyed.
    const TickRate slow{30};
    CHECK(extrapolatePosition(pos, vel, 1.f, slow).x > extrapolatePosition(pos, vel, 1.f, kServerTickRate).x);
}

TEST_CASE("the inset view targets the extrapolated position, not the raw one", "[inset][extrapolate]") {
    // The coincidence property: whatever the renderer draws at, the inset camera looks at. Asserted
    // through the public entry point rather than by re-deriving the offset here.
    const glm::dvec3 tgtPos{500.0, 100.0, 0.0};
    const glm::vec3 tgtVel{300.f, 0.f, 0.f};
    const glm::dvec3 ownPos{0.0, 100.0, 0.0};
    const float alpha = 0.5f;

    const CameraView v = buildTargetInsetView(tgtPos, tgtVel, alpha, ownPos, glm::vec3{0, 1, 0}, 1.5f);
    const glm::dvec3 extrapolated = extrapolatePosition(tgtPos, tgtVel, alpha, kServerTickRate);

    // worldOrigin is the eye. It sits one standoff behind the EXTRAPOLATED target along the
    // target->ownship line, so that distance is the standoff...
    CHECK(glm::length(v.worldOrigin - extrapolated) == Catch::Approx(30.0).margin(1e-6));
    // ...and measurably NOT from the un-extrapolated position, which is what proves the
    // extrapolation ran. The target moves 300 m/s * 0.5 * (1/60) = 2.5 m further from the ownship in
    // half a tick, and the eye is anchored to where it moved TO, so it ends up 27.5 m from where the
    // target was. Pinning the number rather than an inequality also pins the direction.
    CHECK(glm::length(v.worldOrigin - tgtPos) == Catch::Approx(27.5).margin(1e-6));
}
