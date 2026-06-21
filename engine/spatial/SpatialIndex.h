// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fl {

// 2D uniform spatial hash for entity neighbor and range queries.
// XZ-plane bucketing; Y altitude is stored and forwarded to the callback.
// Thread model: sim-thread only. Rebuild each tick: clear() + insert() all live entities.
//
// Algorithm choice: flight-sim entities distribute uniformly across large areas by the nature
// of flight dynamics, so a uniform grid stays O(cells x local_density) on range queries. A
// tree-based index (quad-tree, k-d tree) only outperforms this when N > ~10,000 entities in
// highly non-uniform configurations — well beyond the planned entity scale of this sim.
// See docs/architecture.md (engine-spatial section) for the full rationale.
//
// No ISpatialIndex interface: queryRadius is a function template (Fn&&) that cannot be
// virtualized without erasing Fn to std::function, adding allocator overhead on every
// per-entity callback at 60 Hz. Swapping to a different algorithm means replacing the body
// of SpatialIndex.cpp only — no consumer changes needed, same flexibility as an interface
// at zero runtime cost.
class SpatialIndex {
  public:
    explicit SpatialIndex(double cellSizeM = 10000.0) noexcept;

    // Remove all entries. Call before each per-tick rebuild.
    void clear() noexcept;

    // Insert an entity at the given world position.
    void insert(uint32_t entityIdx, const double pos[3]);

    // Visit all entities in cells intersecting [center ± radiusM] in XZ.
    // Conservative: may include entities in corner cells with exact XZ dist > radiusM.
    // Exact distance filtering is the caller's responsibility.
    // fn: void(uint32_t entityIdx, const double* pos)
    template <typename Fn> void queryRadius(const double center[3], double radiusM, Fn&& fn) const {
        const auto x0 = static_cast<int64_t>(std::floor((center[0] - radiusM) / m_cellSize));
        const auto x1 = static_cast<int64_t>(std::floor((center[0] + radiusM) / m_cellSize));
        const auto z0 = static_cast<int64_t>(std::floor((center[2] - radiusM) / m_cellSize));
        const auto z1 = static_cast<int64_t>(std::floor((center[2] + radiusM) / m_cellSize));
        for (int64_t cx = x0; cx <= x1; ++cx) {
            for (int64_t cz = z0; cz <= z1; ++cz) {
                const auto it = m_grid.find({cx, cz});
                if (it != m_grid.end()) {
                    for (const auto& e : it->second)
                        fn(e.idx, e.pos);
                }
            }
        }
    }

    [[nodiscard]] size_t entityCount() const noexcept {
        return m_count;
    }
    [[nodiscard]] double cellSizeM() const noexcept {
        return m_cellSize;
    }

  private:
    using CellKey = std::pair<int64_t, int64_t>;
    struct CellKeyHash {
        size_t operator()(const CellKey& k) const noexcept; // defined in SpatialIndex.cpp
    };
    struct Entry {
        uint32_t idx;
        double pos[3];
    };

    double m_cellSize;
    std::unordered_map<CellKey, std::vector<Entry>, CellKeyHash> m_grid;
    size_t m_count{0};
};

} // namespace fl
