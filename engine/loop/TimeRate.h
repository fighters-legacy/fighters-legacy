// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fl {

// Time compression factor applied to wall-clock delta before it enters the accumulator.
// At Normal (1×), sim time advances at the same rate as wall time.
// At Octa (8×), 1 real second advances 8 seconds of sim time (more ticks per second).
// At Half (½×), the sim runs at half real-time speed (fewer ticks per second).
enum class TimeRate {
    Paused, // multiplier = 0.0  — sim time does not advance
    Half,   // multiplier = 0.5
    Normal, // multiplier = 1.0
    Double, // multiplier = 2.0
    Quad,   // multiplier = 4.0
    Octa,   // multiplier = 8.0
};

constexpr double timeRateMultiplier(TimeRate r) noexcept {
    switch (r) {
    case TimeRate::Paused:
        return 0.0;
    case TimeRate::Half:
        return 0.5;
    case TimeRate::Normal:
        return 1.0;
    case TimeRate::Double:
        return 2.0;
    case TimeRate::Quad:
        return 4.0;
    case TimeRate::Octa:
        return 8.0;
    }
    return 1.0; // unreachable; silences MSVC C4715
}

} // namespace fl
