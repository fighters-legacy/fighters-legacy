// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cassert>
#include <vector>

namespace fl {

// Bilinear interpolation over a 2D breakpoint table.
// rows = first axis (e.g. alpha), cols = second axis (e.g. Mach).
// Values are stored row-major: values[row * col_count + col].
// Values outside either range are clamped (no extrapolation).
struct Table2D {
    std::vector<float> rows;   // first-axis breakpoints
    std::vector<float> cols;   // second-axis breakpoints
    std::vector<float> values; // row-major, size = rows.size() * cols.size()

    float lookup(float row_val, float col_val) const {
        assert(rows.size() >= 2 && cols.size() >= 2);
        assert(values.size() == rows.size() * cols.size());

        // Clamp to grid extents
        row_val = std::clamp(row_val, rows.front(), rows.back());
        col_val = std::clamp(col_val, cols.front(), cols.back());

        auto row_it = std::lower_bound(rows.begin(), rows.end(), row_val);
        auto col_it = std::lower_bound(cols.begin(), cols.end(), col_val);

        std::size_t r1 = static_cast<std::size_t>(row_it - rows.begin());
        std::size_t c1 = static_cast<std::size_t>(col_it - cols.begin());

        // lower_bound returns the first element >= value; step back for the lower bracket
        if (r1 > 0 && rows[r1] > row_val)
            --r1;
        if (c1 > 0 && cols[c1] > col_val)
            --c1;

        std::size_t r2 = std::min(r1 + 1, rows.size() - 1);
        std::size_t c2 = std::min(c1 + 1, cols.size() - 1);

        float dr = (rows[r2] > rows[r1]) ? (row_val - rows[r1]) / (rows[r2] - rows[r1]) : 0.f;
        float dc = (cols[c2] > cols[c1]) ? (col_val - cols[c1]) / (cols[c2] - cols[c1]) : 0.f;

        std::size_t nc = cols.size();
        float v00 = values[r1 * nc + c1];
        float v01 = values[r1 * nc + c2];
        float v10 = values[r2 * nc + c1];
        float v11 = values[r2 * nc + c2];

        float top = v00 + dc * (v01 - v00);
        float bottom = v10 + dc * (v11 - v10);
        return top + dr * (bottom - top);
    }
};

} // namespace fl
