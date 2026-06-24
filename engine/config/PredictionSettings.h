// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fl {

struct PredictionSettings {
    bool enabled{true};
    float snapThresholdM{5.0f}; // divergence (m) above which reconciliation hard-snaps vs. blends
    float blendRate{0.1f};      // lerp factor [0,1] per reconciliation for soft positional correction
};

} // namespace fl
