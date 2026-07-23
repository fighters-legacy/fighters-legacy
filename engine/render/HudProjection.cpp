// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/HudProjection.h"

namespace fl {

std::optional<glm::vec2> worldToHud(const CameraView& cam, const glm::dvec3& worldPos) {
    // Rebase in DOUBLE first (the float32-safety step): the camera-relative offset is small even at
    // planet scale, so casting it to float after the subtraction avoids the precision loss of
    // transforming an absolute 1e7 m position through a float matrix.
    const glm::vec3 rel = glm::vec3(worldPos - cam.worldOrigin);
    const glm::vec4 clip = cam.proj * (cam.view * glm::vec4(rel, 1.0f));

    // clip.w = -z_view (proj[2][3] = -1): positive iff the point is in front of the camera. This is
    // the correct behind-camera reject for the infinite reverse-Z projection, and it also catches the
    // w ~= 0 grazing case that a depth test would miss.
    if (clip.w <= 0.0f)
        return std::nullopt;

    const glm::vec2 ndc = glm::vec2(clip.x, clip.y) / clip.w;
    // The Vulkan Y-flip is already baked into proj[1][1] = -f, so world-up projects to ndc.y < 0.
    // Mapping [-1,1] -> [0,1] directly therefore yields top-left-origin HUD coords (y grows downward).
    return glm::vec2(ndc.x * 0.5f + 0.5f, ndc.y * 0.5f + 0.5f);
}

std::array<HudElement, 4> hudBox(glm::vec2 center, glm::vec2 halfSize, float r, float g, float b, float a,
                                 float strokeWidth) {
    const float x0 = center.x - halfSize.x, x1 = center.x + halfSize.x;
    const float y0 = center.y - halfSize.y, y1 = center.y + halfSize.y;
    auto line = [&](float ax, float ay, float bx, float by) {
        HudElement e;
        e.type = HudElement::Type::Line;
        e.x = ax;
        e.y = ay;
        e.x2 = bx;
        e.y2 = by;
        e.strokeWidth = strokeWidth;
        e.r = r;
        e.g = g;
        e.b = b;
        e.a = a;
        return e;
    };
    return {line(x0, y0, x1, y0),  // top
            line(x1, y0, x1, y1),  // right
            line(x1, y1, x0, y1),  // bottom
            line(x0, y1, x0, y0)}; // left
}

} // namespace fl
