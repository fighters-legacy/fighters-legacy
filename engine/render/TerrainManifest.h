// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace fl {

// Describes the coordinate mapping and extent of a terrain dataset.
// Shared by TerrainStreamer (#173) and BuiltinGeometry.
struct TerrainManifest {
    std::string terrainId; // canonical terrain ID, e.g. "world"
    float chunkSizeM;      // physical chunk size in metres (typically 15360.0)
    int gridWidth;         // chunk column count; -1 = unbounded (procedural)
    int gridHeight;        // chunk row count;    -1 = unbounded (procedural)
    double originX;        // engine world X of the SW corner of chunk [0, 0]
    double originZ;        // engine world Z of the SW corner of chunk [0, 0]
};

} // namespace fl
