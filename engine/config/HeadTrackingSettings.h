// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fl {

// Head-tracking configuration (#927). Persisted as [headtracking] in user.toml. The tracker speaks the
// opentrack UDP protocol (6 little-endian doubles on a localhost port), which TrackIR, opentrack, and
// phone trackers all emit. All fields have safe defaults; disabled by default.
struct HeadTrackingSettings {
    bool enabled{false};
    int port{4242}; // clamped [1, 65535]
    float yawScale{1.0f};
    float pitchScale{1.0f};
    float rollScale{1.0f};
    float positionalScale{1.0f}; // scales the translational offset (metres)
    bool invertYaw{false};
    bool invertPitch{false};
    bool invertRoll{false};
    float smoothing{0.5f}; // EMA base per 1/60 s frame; clamped [0, 0.95]; 0 = raw
};

} // namespace fl
