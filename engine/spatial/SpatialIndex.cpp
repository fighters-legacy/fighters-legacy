// SPDX-License-Identifier: GPL-3.0-or-later
#include "spatial/SpatialIndex.h"

#include <cassert>

namespace fl {

SpatialIndex::SpatialIndex(double cellSizeM) noexcept : m_cellSize(cellSizeM) {
    assert(cellSizeM > 0.0); // guards UBSan: floor(pos/0.0) is UB; caught by asan.yml
}

size_t SpatialIndex::CellKeyHash::operator()(const CellKey& k) const noexcept {
    // Boost hash_combine — all arithmetic in size_t.
    // 0x9e3779b9u is unsigned int; promotes to size_t in the expression.
    // Matches TerrainStreamer::ChunkKeyHash (engine/render/TerrainStreamer.cpp:24-29).
    size_t h = std::hash<int64_t>{}(k.first);
    h ^= std::hash<int64_t>{}(k.second) + 0x9e3779b9u + (h << 6) + (h >> 2);
    return h;
}

void SpatialIndex::clear() {
    // Recycle each cell's vector (retaining its capacity) into the spare pool instead of freeing it,
    // so the next rebuild's inserts reuse the allocations. Bounded by the peak live cell count.
    for (auto& [key, bucket] : m_grid) {
        bucket.clear();
        m_spare.push_back(std::move(bucket));
    }
    m_grid.clear();
    m_count = 0;
}

void SpatialIndex::setCellSize(double cellSizeM) {
    assert(cellSizeM > 0.0);
    clear();
    m_cellSize = cellSizeM;
}

void SpatialIndex::insert(uint32_t entityIdx, const double pos[3]) {
    const auto cx = static_cast<int64_t>(std::floor(pos[0] / m_cellSize));
    const auto cz = static_cast<int64_t>(std::floor(pos[2] / m_cellSize));
    const CellKey key{cx, cz};
    auto it = m_grid.find(key);
    if (it == m_grid.end()) {
        std::vector<Entry> bucket;
        if (!m_spare.empty()) {
            bucket = std::move(m_spare.back()); // reuse a capacity-retaining buffer
            m_spare.pop_back();
        }
        it = m_grid.emplace(key, std::move(bucket)).first;
    }
    it->second.push_back({entityIdx, {pos[0], pos[1], pos[2]}});
    ++m_count;
}

} // namespace fl
