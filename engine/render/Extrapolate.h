// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net/TickRate.h" // the ONE ticks-to-wall-time conversion (#1075)

#include <glm/glm.hpp>

namespace fl {

// Sub-tick position extrapolation, in one place (#1250).
//
// Six sites spelled this out: the scene renderer, four camera paths, and the target inset view.
// They MUST agree -- the camera target is set from the same snapshot as the rendered entity
// specifically so the two are coincident, and a divergent copy detaches the camera from the thing
// it is looking at. That is not hypothetical: InsetViewMath already records a hardcoded 1/60 that
// "would disagree with the alpha on any server not stepping at 60 Hz", which is the #1075 fix.
//
// One body makes the coincidence structural rather than a property six edits have to preserve.
//
// `alpha` is how far through a SERVER tick the frame is, so the period it multiplies has to be that
// server's period, not the client's frame time. Header-only over TickRate; no link edge either way.
[[nodiscard]] inline glm::dvec3 extrapolatePosition(const glm::dvec3& pos, const glm::vec3& vel, float alpha,
                                                    TickRate rate) noexcept {
    return pos + glm::dvec3(vel * (alpha * rate.dtSeconds()));
}

} // namespace fl
