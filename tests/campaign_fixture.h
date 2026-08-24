// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The synthetic frontline loader both campaign suites drive the engine with (#1276).
//
// Two copies existed, semantically identical -- the same 8x4 default raster, the same `after` keying,
// the same 60/200 half-split -- and had already diverged in spelling: one built the split through a
// helper, the other inline, and only one could count loads. The union is one function with an
// optional counter, which is exactly what the two call sites needed.

#include "campaign/CampaignEngine.h"
#include "campaign/Frontline.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fl {

// A loader that hands out a synthetic raster keyed by path, so the engine never needs a real PNG.
// West half side A (60), east half side B (200); a path containing "after" yields all side A, which
// is how a suite says "the story objective was won". Pass loadCount to observe how often the engine
// actually reaches for a raster.
[[nodiscard]] inline CampaignEngine::FrontlineLoader syntheticLoader(int* loadCount = nullptr) {
    return [loadCount](const std::string& path, Frontline& out) -> bool {
        if (loadCount)
            ++*loadCount;
        const int cols = out.cols() > 0 ? out.cols() : 8;
        const int rows = out.rows() > 0 ? out.rows() : 4;
        Frontline f(cols, rows, out.bounds());
        std::vector<uint8_t> px(static_cast<std::size_t>(cols) * rows, 0);
        if (path.find("after") != std::string::npos) {
            px.assign(px.size(), 60); // all side A after the story win
        } else {
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c)
                    px[static_cast<std::size_t>(r) * cols + c] = (c < cols / 2) ? 60 : 200;
        }
        (void)f.setPixels(std::move(px));
        out = std::move(f);
        return true;
    };
}

} // namespace fl
