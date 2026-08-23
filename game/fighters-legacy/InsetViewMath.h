// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/Extrapolate.h" // the one sub-tick extrapolation (#1250)

#include "net/TickRate.h"            // server tick rate paired with the render alpha (#1075)
#include "render/CameraController.h" // makeCameraView

#include <glm/glm.hpp>

namespace fl {

// Target-slaved inset camera math (#698), pure so it is unit-testable without a renderer. Places the
// eye on the target->ownship line at a fixed stand-off behind the target and looks at the velocity-
// extrapolated target. Shares makeCameraView() with CameraController::view() so projection conventions
// cannot diverge.
[[nodiscard]] inline CameraView buildTargetInsetView(const glm::dvec3& targetPos, const glm::vec3& targetVel,
                                                     float renderAlpha, const glm::dvec3& ownPos, glm::vec3 worldUp,
                                                     float rectAspect, double standoffM = 30.0,
                                                     TickRate serverTickRate = kServerTickRate) {
    // renderAlpha is "how far through a SERVER tick", so the period it multiplies must be that
    // server's period (#1075) — a hardcoded 1/60 here would disagree with the alpha on any server
    // not stepping at 60 Hz. Defaulted so the pure-math call sites in tests stay one-liners.
    const glm::dvec3 extTgt = extrapolatePosition(targetPos, targetVel, renderAlpha, serverTickRate);

    // Eye behind the target along the target->ownship line. Degenerate (ownship on target): offset by
    // worldUp x an arbitrary axis so the look direction stays well-defined.
    glm::dvec3 toOwn = ownPos - extTgt;
    double d = glm::length(toOwn);
    glm::dvec3 dir = (d > 1e-3) ? toOwn / d : glm::dvec3(glm::normalize(glm::cross(worldUp, glm::vec3{1, 0, 0})));
    const glm::dvec3 eye = extTgt + dir * standoffM;
    const glm::vec3 forward = glm::vec3(extTgt - eye); // look at the target
    return makeCameraView(eye, forward, worldUp, rectAspect);
}

// The inset's normalized HUD rect (top-left origin): bottom-centre, ~22% of width, square-pixel given
// the window aspect. Shared so FlightScreen's border and the renderer's viewport use the same rect.
[[nodiscard]] inline glm::vec4 insetRectFor(float windowAspect) {
    constexpr float w = 0.22f;
    const float h = w * windowAspect; // square pixels: normalized-h = normalized-w * (width/height)
    const float x = 0.5f - w * 0.5f;
    const float y = 0.97f - h; // bottom-centre, small margin from the edge
    return glm::vec4{x, y, w, h};
}

} // namespace fl
