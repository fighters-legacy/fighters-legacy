// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h" // CameraView, HudElement

#include <array>
#include <glm/glm.hpp>
#include <optional>

// World-to-HUD projection (#692). No world->screen projection existed anywhere; padlock status cues,
// the #641 target designator box / pipper / CCIP, and future target labels all need one. Shipped once
// as pure, testable math that mirrors CameraController::view()'s exact conventions (infinite reverse-Z
// with the Vulkan Y-flip baked into proj[1][1] = -f), so the HUD and the renderer can never diverge.

namespace fl {

// Project an absolute world position into normalized top-left-origin HUD coordinates ([0,1] on-screen;
// callers may receive values outside [0,1] and must cull/clamp). Returns nullopt when the point is at
// or behind the camera plane — with the infinite reverse-Z projection, clip.w <= 0 is the correct
// behind-camera test (clip.w = -z_view), NOT a depth comparison.
[[nodiscard]] std::optional<glm::vec2> worldToHud(const CameraView& cam, const glm::dvec3& worldPos);

// Four HudElement::Line commands tracing an axis-aligned box (the target designator, #641). There is
// no outline-rect HudElement type, so a box is four lines. center/halfSize are normalized HUD coords.
[[nodiscard]] std::array<HudElement, 4> hudBox(glm::vec2 center, glm::vec2 halfSize, float r, float g, float b, float a,
                                               float strokeWidth = 1.0f);

// Recover the viewport aspect (width/height) from a CameraView's projection: proj[0][0] = f/aspect
// and proj[1][1] = -f, so aspect = -proj[1][1] / proj[0][0]. Lets HUD code that needs the aspect for
// square symbology read it from the live camera instead of hard-coding 16/9.
[[nodiscard]] inline float hudAspect(const CameraView& cam) {
    const float f = -cam.proj[1][1];
    const float fOverAspect = cam.proj[0][0];
    return (fOverAspect > 1e-6f) ? f / fOverAspect : 1.0f;
}

} // namespace fl
