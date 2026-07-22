// SPDX-License-Identifier: GPL-3.0-or-later
//
// World-to-HUD projection tests (#692). Golden values are checked against a CameraView built by the
// real CameraController::view(), so the helper stays in lockstep with the renderer's reverse-Z /
// Y-flip / camera-relative conventions rather than a hand-transcribed matrix.

#include "render/CameraController.h"
#include "render/HudProjection.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

using namespace fl;

namespace {
// A camera at `eye` looking along -Z with world +Y up (the canonical setup), viewport aspect `aspect`.
CameraView makeCam(glm::dvec3 eye, float aspect) {
    CameraController cc;
    cc.setPose(eye, glm::vec3{0.f, 0.f, -1.f}, glm::vec3{0.f, 1.f, 0.f});
    return cc.view(aspect);
}
} // namespace

TEST_CASE("worldToHud: a point on the view axis projects to screen centre (#692)", "[hud_projection]") {
    const CameraView cam = makeCam({0.0, 0.0, 0.0}, 16.f / 9.f);
    auto p = worldToHud(cam, glm::dvec3{0.0, 0.0, -100.0}); // 100 m dead ahead
    REQUIRE(p.has_value());
    CHECK(p->x == Catch::Approx(0.5f).margin(1e-5f));
    CHECK(p->y == Catch::Approx(0.5f).margin(1e-5f));
}

TEST_CASE("worldToHud: world-up projects above centre, camera-right projects right of centre (#692)",
          "[hud_projection]") {
    const CameraView cam = makeCam({0.0, 0.0, 0.0}, 16.f / 9.f);
    // A point up and ahead -> screen y < 0.5 (top-left origin, y grows downward).
    auto up = worldToHud(cam, glm::dvec3{0.0, 20.0, -100.0});
    REQUIRE(up.has_value());
    CHECK(up->y < 0.5f);
    CHECK(up->x == Catch::Approx(0.5f).margin(1e-5f));
    // A point to the camera's right (+X) and ahead -> screen x > 0.5.
    auto right = worldToHud(cam, glm::dvec3{20.0, 0.0, -100.0});
    REQUIRE(right.has_value());
    CHECK(right->x > 0.5f);
    CHECK(right->y == Catch::Approx(0.5f).margin(1e-5f));
}

TEST_CASE("worldToHud: behind-camera and grazing points return nullopt (#692)", "[hud_projection]") {
    const CameraView cam = makeCam({0.0, 0.0, 0.0}, 16.f / 9.f);
    CHECK_FALSE(worldToHud(cam, glm::dvec3{0.0, 0.0, 100.0}).has_value()); // straight behind
    CHECK_FALSE(worldToHud(cam, glm::dvec3{0.0, 0.0, 0.0}).has_value());   // at the eye (w == 0)
    CHECK_FALSE(worldToHud(cam, glm::dvec3{5.0, 0.0, 0.0}).has_value());   // on the camera plane
}

TEST_CASE("worldToHud: an off-axis on-screen point stays within [0,1], far off-axis leaves it (#692)",
          "[hud_projection]") {
    const CameraView cam = makeCam({0.0, 0.0, 0.0}, 16.f / 9.f);
    // Small angle off-axis at 100 m -> on screen.
    auto onScreen = worldToHud(cam, glm::dvec3{5.0, 3.0, -100.0});
    REQUIRE(onScreen.has_value());
    CHECK(onScreen->x > 0.0f);
    CHECK(onScreen->x < 1.0f);
    CHECK(onScreen->y > 0.0f);
    CHECK(onScreen->y < 1.0f);
    // A large lateral offset close in -> projects OUTSIDE [0,1] (still in front, so not nullopt).
    auto offScreen = worldToHud(cam, glm::dvec3{400.0, 0.0, -100.0});
    REQUIRE(offScreen.has_value());
    CHECK(offScreen->x > 1.0f);
}

TEST_CASE("worldToHud: aspect ratio scales the horizontal projection (#692)", "[hud_projection]") {
    // The same off-axis point projects further from centre horizontally at a narrower aspect.
    auto wide = worldToHud(makeCam({0.0, 0.0, 0.0}, 21.f / 9.f), glm::dvec3{20.0, 0.0, -100.0});
    auto narrow = worldToHud(makeCam({0.0, 0.0, 0.0}, 4.f / 3.f), glm::dvec3{20.0, 0.0, -100.0});
    REQUIRE(wide.has_value());
    REQUIRE(narrow.has_value());
    // Narrower aspect -> larger f/aspect... no: proj[0][0] = f/aspect, so a SMALLER aspect gives a
    // LARGER x deflection. 4:3 < 21:9, so narrow is further from centre.
    CHECK((narrow->x - 0.5f) > (wide->x - 0.5f));
}

TEST_CASE("worldToHud: planet-scale positions show no precision jitter (#692)", "[hud_projection]") {
    // Camera far from the world origin; the double rebase must keep a 100 m-ahead point centred.
    const glm::dvec3 eye{3.0e6, 1.0e4, -5.0e6};
    const CameraView cam = makeCam(eye, 16.f / 9.f);
    auto ahead = worldToHud(cam, eye + glm::dvec3{0.0, 0.0, -100.0});
    REQUIRE(ahead.has_value());
    CHECK(ahead->x == Catch::Approx(0.5f).margin(1e-4f));
    CHECK(ahead->y == Catch::Approx(0.5f).margin(1e-4f));
}

TEST_CASE("hudAspect recovers the constructor aspect; hudBox traces the rectangle (#692)", "[hud_projection]") {
    CHECK(hudAspect(makeCam({0.0, 0.0, 0.0}, 16.f / 9.f)) == Catch::Approx(16.f / 9.f).margin(1e-4f));
    CHECK(hudAspect(makeCam({0.0, 0.0, 0.0}, 4.f / 3.f)) == Catch::Approx(4.f / 3.f).margin(1e-4f));

    const auto box = hudBox(glm::vec2{0.5f, 0.5f}, glm::vec2{0.1f, 0.2f}, 0.f, 1.f, 0.f, 1.f);
    for (const auto& e : box) {
        CHECK(e.type == HudElement::Type::Line);
        // Every endpoint lies on the box border.
        CHECK((e.x == Catch::Approx(0.4f) || e.x == Catch::Approx(0.6f)));
        CHECK((e.y == Catch::Approx(0.3f) || e.y == Catch::Approx(0.7f)));
    }
}
