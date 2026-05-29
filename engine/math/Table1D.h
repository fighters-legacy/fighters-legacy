// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cassert>
#include <vector>

namespace fl {

// Linear interpolation over a 1D breakpoint table.
// Values outside the breakpoint range are clamped (no extrapolation).
struct Table1D {
    std::vector<float> keys;
    std::vector<float> values;

    float lookup(float key) const {
        assert(keys.size() >= 2);
        assert(keys.size() == values.size());

        if (key <= keys.front())
            return values.front();
        if (key >= keys.back())
            return values.back();

        auto it = std::lower_bound(keys.begin(), keys.end(), key);
        auto hi = static_cast<std::size_t>(it - keys.begin());
        auto lo = hi - 1;

        float t = (key - keys[lo]) / (keys[hi] - keys[lo]);
        return values[lo] + t * (values[hi] - values[lo]);
    }
};

} // namespace fl
