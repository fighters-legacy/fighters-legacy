// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The client's frame delta, in seconds — the one every frame-driven system integrates (#1241).
//
// Ten sites passed a literal 1/60 as an assumed FRAME dt: the head tracker, the windshield rain,
// the haptic controller, the effect router, voice, subtitles, music, warning tones and a landed-
// still timer. None of them is sim-side, so the server tick rate is the wrong substitute; they run
// once per rendered frame and therefore need the duration of that frame. At any rate other than 60
// they were simply wrong — timers ran fast at 144 Hz and slow at 30, rain and particles advanced
// per frame rather than per second, and HapticController's G-load, which DIVIDES by dt, reported a
// number scaled by the ratio of real to assumed frame time.
//
// ⚠ CLAMPED, and the clamp is the load-bearing part. `frameDtMs` is documented UNCAPPED wall clock
// (platform/RenderTypes.h), so a hitch, an alt-tab, a shader compile or a breakpoint hands the next
// frame a delta measured in seconds. Feeding that raw into a timer or an integrator is a worse bug
// than the bounded-but-wrong 1/60 it replaces — it teleports state. The ceiling turns a stall into
// a slow frame rather than a jump. The floor keeps the divide in HapticController's G-load finite:
// a zero or near-zero delta (the first frame, or a stats source that has not measured one yet)
// would otherwise produce an enormous G-load and fire the G-LOC effect on a stationary aircraft.
//
// Deliberately NOT a running average. These consumers want the time that actually passed; smoothing
// belongs to whoever wants smoothing, and a shared helper that quietly filtered would be the next
// thing nobody could find.

#include "IRenderer.h"

#include <algorithm>

namespace fl {

// A frame slower than this is treated as this long. 100 ms = 10 fps; below that the client is not
// really running and pretending otherwise only corrupts state.
inline constexpr float kMaxFrameDtS = 0.1f;
// And no shorter than a millisecond, so anything dividing by dt stays finite.
inline constexpr float kMinFrameDtS = 0.001f;
// What a frame is assumed to have taken before anything has measured one.
inline constexpr float kNominalFrameDtS = 1.0f / 60.0f;

// The duration of the frame just ended, clamped. `renderer` may be null (a headless or
// still-initialising client), which yields the nominal step.
[[nodiscard]] inline float frameDeltaSeconds(const IRenderer* renderer) {
    if (!renderer)
        return kNominalFrameDtS;
    const float dtS = renderer->getFrameStats().frameDtMs * 0.001f;
    if (!(dtS > 0.0f)) // also catches NaN
        return kNominalFrameDtS;
    return std::clamp(dtS, kMinFrameDtS, kMaxFrameDtS);
}

} // namespace fl
