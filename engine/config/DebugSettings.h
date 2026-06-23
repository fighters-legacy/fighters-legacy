// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

// Persisted under the [debug] section of user.toml.
enum class OverlayMode : uint8_t {
    Off = 0,
    Compact = 1, // FPS + frame time on one line
    Full = 2,    // all stats + rolling bar graph
};

struct DebugSettings {
    OverlayMode overlayMode{OverlayMode::Off};
};

} // namespace fl
