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
