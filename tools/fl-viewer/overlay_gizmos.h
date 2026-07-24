// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"

#include <glm/glm.hpp>
#include <vector>

namespace fl {

// Pure 2D-overlay builders for the fl-viewer (#838). They project world-space guide geometry through
// the camera (HudProjection::worldToHud) into HudElement lines/text, so they need no renderer changes
// and are unit-testable with a canned CameraView. A segment with either endpoint behind the camera is
// dropped.

// A grid on the XZ plane at Y=0: (2*halfLines+1) lines each way, `spacingM` apart, centred on origin.
[[nodiscard]] std::vector<HudElement> buildGridOverlay(const CameraView& cam, float spacingM, int halfLines,
                                                       glm::vec4 color);

// Auto-pick a grid spacing (1 / 5 / 10 / 50 m ...) from the model's bounding radius so ~10 lines span it.
[[nodiscard]] float autoGridSpacing(float boundsRadiusM);

// The engine-axis gizmo: three colored lines from the origin — +X red (nose), +Y green (up), +Z blue
// (starboard) — of length `axisLenM`, each with a text label ("X"/"Y"/"Z") at its tip.
[[nodiscard]] std::vector<HudElement> buildAxisGizmo(const CameraView& cam, float axisLenM);

} // namespace fl
