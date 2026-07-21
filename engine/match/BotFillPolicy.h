// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>

namespace fl {

// How many AI bots the server should run to reach `fill` total participants given `humans` humans,
// capped at `maxBots` (#87). Pure. `fill` 0 = bots disabled.
inline int desiredBots(int humans, int fill, int maxBots) {
    if (fill <= 0)
        return 0;
    return std::clamp(fill - std::max(0, humans), 0, std::max(0, maxBots));
}

} // namespace fl
