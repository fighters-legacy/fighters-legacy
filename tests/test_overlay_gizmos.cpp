// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "overlay_gizmos.h"
#include "render/CameraController.h"

#include <glm/glm.hpp>

using namespace fl;

namespace {
// A camera above-and-behind the origin, looking at it — so +X/+Y/+Z all project in front.
CameraView cam() {
    return makeCameraView(glm::dvec3(6.0, 5.0, 8.0), glm::vec3(-6.0f, -5.0f, -8.0f), glm::vec3(0, 1, 0), 16.0f / 9.0f);
}
} // namespace

TEST_CASE("autoGridSpacing rounds to a 1/2/5 * 10^n step", "[gizmos]") {
    CHECK(autoGridSpacing(5.0f) == Catch::Approx(1.0f));  // 2*5/10 = 1
    CHECK(autoGridSpacing(30.0f) == Catch::Approx(5.0f)); // 2*30/10 = 6 -> 5
    CHECK(autoGridSpacing(0.0f) == Catch::Approx(1.0f));  // degenerate -> 1
}

TEST_CASE("buildGridOverlay emits lines on the Y=0 plane", "[gizmos]") {
    auto grid = buildGridOverlay(cam(), 2.0f, 3, {0.4f, 0.4f, 0.45f, 0.5f});
    // 2*halfLines+1 = 7 lines each direction = up to 14; some may be culled behind the camera.
    CHECK(!grid.empty());
    CHECK(grid.size() <= 14);
    for (const auto& e : grid)
        CHECK(e.type == HudElement::Type::Line);

    // Zero spacing / no lines -> empty.
    CHECK(buildGridOverlay(cam(), 0.0f, 3, {}).empty());
    CHECK(buildGridOverlay(cam(), 2.0f, 0, {}).empty());
}

TEST_CASE("buildAxisGizmo emits 3 axis lines + 3 labels", "[gizmos]") {
    auto g = buildAxisGizmo(cam(), 4.0f);
    int lines = 0, texts = 0;
    for (const auto& e : g) {
        if (e.type == HudElement::Type::Line)
            ++lines;
        else if (e.type == HudElement::Type::Text)
            ++texts;
    }
    CHECK(lines == 3);
    CHECK(texts == 3);
    // The three axis lines are red / green / blue (approximately).
    bool haveRed = false, haveGreen = false, haveBlue = false;
    for (const auto& e : g) {
        if (e.type != HudElement::Type::Line)
            continue;
        if (e.r > 0.8f && e.g < 0.4f && e.b < 0.4f)
            haveRed = true;
        if (e.g > 0.8f && e.r < 0.4f && e.b < 0.4f)
            haveGreen = true;
        if (e.b > 0.8f && e.r < 0.5f)
            haveBlue = true;
    }
    CHECK(haveRed);
    CHECK(haveGreen);
    CHECK(haveBlue);
}
