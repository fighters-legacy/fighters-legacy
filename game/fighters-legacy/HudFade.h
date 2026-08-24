// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fl {

// The timed-HUD-line fade curve, in one place (#1265).
//
// Game-local on purpose. The two consumers -- ChatOverlay and KillFeed -- are both client HUD
// overlays, and KillFeed's test deliberately links nothing but Catch2 and platform-hal, so putting
// this in engine/render would have forced that test to grow an engine dependency to keep compiling.
// A shared home is only shared if everyone who needs it can reach it.

// Opacity of a timed HUD line: fully opaque for `dwellSecs`, then linear to nothing over
// `fadeSecs` (#1265).
//
// The chat overlay and the kill feed are the two timed line-stacks, and both spelled this out. Their
// dwell and fade DIFFER on purpose (10/2 versus 8/2) -- that is what makes them parameters rather
// than constants -- but the curve is one rule, and two copies of it is two chances for one stack to
// snap off while the other fades.
[[nodiscard]] inline float fadeAlpha(float ageSecs, float dwellSecs, float fadeSecs) noexcept {
    if (ageSecs <= dwellSecs)
        return 1.f;
    if (fadeSecs <= 0.f)
        return 0.f; // no fade window: the line ends the instant its dwell does
    const float t = 1.f - (ageSecs - dwellSecs) / fadeSecs;
    return t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
}

} // namespace fl
