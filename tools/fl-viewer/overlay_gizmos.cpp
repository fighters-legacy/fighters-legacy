// SPDX-License-Identifier: GPL-3.0-or-later
#include "overlay_gizmos.h"

#include "render/HudProjection.h"

#include <cmath>
#include <vector>

namespace fl {

namespace {
// Append a world-space segment as a HudElement line if both ends project in front of the camera.
void addSegment(std::vector<HudElement>& out, const CameraView& cam, const glm::dvec3& a, const glm::dvec3& b,
                glm::vec4 color, float strokePx) {
    auto pa = worldToHud(cam, a);
    auto pb = worldToHud(cam, b);
    if (!pa || !pb)
        return; // an endpoint is behind the camera — drop the segment (no clipping in v1)
    HudElement e{};
    e.type = HudElement::Type::Line;
    e.x = pa->x;
    e.y = pa->y;
    e.x2 = pb->x;
    e.y2 = pb->y;
    e.strokeWidth = strokePx;
    e.r = color.r;
    e.g = color.g;
    e.b = color.b;
    e.a = color.a;
    out.push_back(e);
}
} // namespace

float autoGridSpacing(float boundsRadiusM) {
    // Aim for ~10 grid lines across the model. Round the target up to a 1/2/5 * 10^n step.
    const float target = boundsRadiusM > 0.01f ? (2.0f * boundsRadiusM) / 10.0f : 1.0f;
    const float mag = std::pow(10.0f, std::floor(std::log10(target)));
    const float norm = target / mag; // [1,10)
    const float step = norm <= 1.5f ? 1.0f : norm <= 3.5f ? 2.0f : norm <= 7.5f ? 5.0f : 10.0f;
    return step * mag;
}

std::vector<HudElement> buildGridOverlay(const CameraView& cam, float spacingM, int halfLines, glm::vec4 color) {
    std::vector<HudElement> out;
    if (spacingM <= 0.0f || halfLines < 1)
        return out;
    const double extent = static_cast<double>(spacingM) * halfLines;
    for (int i = -halfLines; i <= halfLines; ++i) {
        const double c = static_cast<double>(spacingM) * i;
        // Lines parallel to X (vary Z) and parallel to Z (vary X), on the Y=0 plane.
        addSegment(out, cam, {-extent, 0.0, c}, {extent, 0.0, c}, color, 1.0f);
        addSegment(out, cam, {c, 0.0, -extent}, {c, 0.0, extent}, color, 1.0f);
    }
    return out;
}

std::vector<HudElement> buildAxisGizmo(const CameraView& cam, float axisLenM) {
    std::vector<HudElement> out;
    const double L = axisLenM;
    const glm::vec4 red{1.0f, 0.2f, 0.2f, 1.0f};   // +X nose
    const glm::vec4 green{0.2f, 1.0f, 0.2f, 1.0f}; // +Y up
    const glm::vec4 blue{0.3f, 0.5f, 1.0f, 1.0f};  // +Z starboard
    addSegment(out, cam, {0, 0, 0}, {L, 0, 0}, red, 2.0f);
    addSegment(out, cam, {0, 0, 0}, {0, L, 0}, green, 2.0f);
    addSegment(out, cam, {0, 0, 0}, {0, 0, L}, blue, 2.0f);

    auto addLabel = [&](const glm::dvec3& at, const char* text, glm::vec4 color) {
        auto p = worldToHud(cam, at);
        if (!p)
            return;
        HudElement e{};
        e.type = HudElement::Type::Text;
        e.x = p->x;
        e.y = p->y;
        e.r = color.r;
        e.g = color.g;
        e.b = color.b;
        e.a = color.a;
        e.text = text; // static string literal — outlives the frame
        out.push_back(e);
    };
    addLabel({L * 1.1, 0, 0}, "X", red);
    addLabel({0, L * 1.1, 0}, "Y", green);
    addLabel({0, 0, L * 1.1}, "Z", blue);
    return out;
}

} // namespace fl
