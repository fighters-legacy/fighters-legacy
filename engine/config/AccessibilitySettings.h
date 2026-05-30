// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Accessibility preferences persisted in config/user.toml under [accessibility].
struct AccessibilitySettings {
    bool subtitlesEnabled{true};
    float subtitleDurationScale{1.0f}; // multiplier applied to all subtitle durations [0.5, 3.0]
};
