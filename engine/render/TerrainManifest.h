// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace fl {

// Describes a cube-sphere terrain dataset (#472). The planar chunk-grid fields
// (chunkSizeM / gridWidth / gridHeight / originX / originZ) were removed with the
// quadtree streamer rewrite: tile addressing is the CubeSphere TileKey quadtree
// (6 faces x 4^level tiles) and the planet radius comes from
// TerrainStreamer::setPlanetRadius(). Shared by TerrainStreamer and BuiltinGeometry.
struct TerrainManifest {
    std::string terrainId; // canonical terrain ID, e.g. "world"
    int maxTileLevel{12};  // deepest quadtree level the streamer refines to
                           // (level 12 on Earth ~ 30 m per mesh quad)
};

} // namespace fl
