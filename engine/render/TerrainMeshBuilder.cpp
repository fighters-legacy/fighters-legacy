// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/TerrainMeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fl {

namespace {

// ---------------------------------------------------------------------------
// Little-endian write helpers (host is always LE on our targets)
// ---------------------------------------------------------------------------

void writeLE32(uint8_t* p, uint32_t v) {
    std::memcpy(p, &v, 4);
}

} // namespace

std::vector<uint8_t> buildTerrainMeshGlb(const std::vector<uint16_t>& heights, int heightmapSize, int meshGrid,
                                         float chunkSizeM) noexcept {
    // Validate input
    if (heights.empty() || heightmapSize < 2 || meshGrid <= 0)
        return {};
    if (heightmapSize < meshGrid + 1)
        return {};
    if (static_cast<int>(heights.size()) < heightmapSize * heightmapSize)
        return {};

    const int stride = (heightmapSize - 1) / meshGrid; // integer, ≥1
    const int gridPts = meshGrid + 1;                  // vertices per side
    const int vertCount = gridPts * gridPts;
    const int quadCount = meshGrid * meshGrid;
    const int indexCount = quadCount * 6;

    const float cellM = chunkSizeM / static_cast<float>(meshGrid);

    // height lookup (clamped)
    auto hAt = [&](int col, int row) -> float {
        col = std::clamp(col, 0, heightmapSize - 1);
        row = std::clamp(row, 0, heightmapSize - 1);
        return static_cast<float>(heights[static_cast<std::size_t>(row) * heightmapSize + col]) - 32768.0f;
    };

    // Build vertex arrays
    std::vector<float> positions(static_cast<std::size_t>(vertCount) * 3);
    std::vector<float> normals(static_cast<std::size_t>(vertCount) * 3);

    float yMin = std::numeric_limits<float>::max();
    float yMax = -std::numeric_limits<float>::max();

    for (int row = 0; row < gridPts; ++row) {
        for (int col = 0; col < gridPts; ++col) {
            const int hc = col * stride;
            const int hr = row * stride;
            const float y = hAt(hc, hr);

            const std::size_t vi = static_cast<std::size_t>(row * gridPts + col);
            positions[vi * 3 + 0] = static_cast<float>(col) * cellM;
            positions[vi * 3 + 1] = y;
            positions[vi * 3 + 2] = static_cast<float>(row) * cellM;

            if (y < yMin)
                yMin = y;
            if (y > yMax)
                yMax = y;

            // Central-difference normal
            const float hl = hAt(hc - stride, hr);
            const float hr_ = hAt(hc + stride, hr);
            const float hu = hAt(hc, hr - stride);
            const float hd = hAt(hc, hr + stride);
            const float dx = hr_ - hl;
            const float dz = hd - hu;
            // sampleSpacing = cellM * 2 (distance between samples for central diff)
            const float sc = cellM * 2.0f;
            // normal = normalize(-dx, sc, -dz)
            const float nx = -dx;
            const float ny = sc;
            const float nz = -dz;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            normals[vi * 3 + 0] = (len > 0.0f) ? nx / len : 0.0f;
            normals[vi * 3 + 1] = (len > 0.0f) ? ny / len : 1.0f;
            normals[vi * 3 + 2] = (len > 0.0f) ? nz / len : 0.0f;
        }
    }

    // Build index array (CCW winding from above)
    std::vector<uint16_t> indices(static_cast<std::size_t>(indexCount));
    std::size_t ii = 0;
    for (int row = 0; row < meshGrid; ++row) {
        for (int col = 0; col < meshGrid; ++col) {
            const uint16_t v = static_cast<uint16_t>(row * gridPts + col);
            const uint16_t vr = static_cast<uint16_t>(v + 1);
            const uint16_t vd = static_cast<uint16_t>(v + gridPts);
            const uint16_t vdr = static_cast<uint16_t>(v + gridPts + 1);
            // tri 0: v, vdr, vd
            indices[ii++] = v;
            indices[ii++] = vdr;
            indices[ii++] = vd;
            // tri 1: v, vr, vdr
            indices[ii++] = v;
            indices[ii++] = vr;
            indices[ii++] = vdr;
        }
    }

    // ---------------------------------------------------------------------------
    // Assemble BIN buffer (non-interleaved)
    // ---------------------------------------------------------------------------
    const std::size_t posBytes = static_cast<std::size_t>(vertCount) * 3 * sizeof(float);
    const std::size_t nrmBytes = posBytes;
    const std::size_t idxBytes = static_cast<std::size_t>(indexCount) * sizeof(uint16_t);
    const std::size_t binBytes = posBytes + nrmBytes + idxBytes;
    // Pad to 4-byte boundary (all our sizes are already multiples of 4, but pad anyway)
    const std::size_t binPadded = (binBytes + 3u) & ~3u;

    std::vector<uint8_t> bin(binPadded, 0);
    std::memcpy(bin.data(), positions.data(), posBytes);
    std::memcpy(bin.data() + posBytes, normals.data(), nrmBytes);
    std::memcpy(bin.data() + posBytes + nrmBytes, indices.data(), idxBytes);

    // ---------------------------------------------------------------------------
    // Build JSON (glTF 2.0)
    // ---------------------------------------------------------------------------
    const std::size_t posOff = 0;
    const std::size_t nrmOff = posBytes;
    const std::size_t idxOff = posBytes + nrmBytes;

    // POSITION min/max for accessor
    const float xMax = static_cast<float>(meshGrid) * cellM;
    const float zMax = xMax;

    std::string json = "{";
    json += R"("asset":{"version":"2.0"},)";
    json += R"("scene":0,)";
    json += R"("scenes":[{"nodes":[0]}],)";
    json += R"("nodes":[{"mesh":0}],)";
    json +=
        R"("meshes":[{"name":"terrain","primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2,"mode":4}]}],)";

    // accessors
    json += "\"accessors\":[";
    // 0: POSITION
    json += "{\"bufferView\":0,\"byteOffset\":0,\"componentType\":5126,\"count\":" + std::to_string(vertCount) +
            ",\"type\":\"VEC3\""
            ",\"min\":[0.0,";
    json += std::to_string(yMin) + ",0.0],\"max\":[" + std::to_string(xMax) + "," + std::to_string(yMax) + "," +
            std::to_string(zMax) + "]},";
    // 1: NORMAL
    json += "{\"bufferView\":1,\"byteOffset\":0,\"componentType\":5126,\"count\":" + std::to_string(vertCount) +
            ",\"type\":\"VEC3\"},";
    // 2: INDICES
    json += "{\"bufferView\":2,\"byteOffset\":0,\"componentType\":5123,\"count\":" + std::to_string(indexCount) +
            ",\"type\":\"SCALAR\"}],";

    // bufferViews
    json += "\"bufferViews\":[";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(posOff) + ",\"byteLength\":" + std::to_string(posBytes) +
            ",\"target\":34962},";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(nrmOff) + ",\"byteLength\":" + std::to_string(nrmBytes) +
            ",\"target\":34962},";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(idxOff) + ",\"byteLength\":" + std::to_string(idxBytes) +
            ",\"target\":34963}],";

    // buffer
    json += "\"buffers\":[{\"byteLength\":" + std::to_string(binPadded) + "}]";
    json += "}";

    // Pad JSON to 4-byte boundary with spaces
    while (json.size() % 4 != 0)
        json += ' ';

    // ---------------------------------------------------------------------------
    // Assemble GLB
    // ---------------------------------------------------------------------------
    // Chunk types
    static constexpr uint32_t kChunkJSON = 0x4E4F534Au;
    static constexpr uint32_t kChunkBIN = 0x004E4942u;
    static constexpr uint32_t kMagic = 0x46546C67u;

    const std::size_t jsonChunkSize = 8 + json.size();
    const std::size_t binChunkSize = 8 + binPadded;
    const std::size_t totalSize = 12 + jsonChunkSize + binChunkSize;

    std::vector<uint8_t> glb(totalSize, 0);
    uint8_t* p = glb.data();

    // GLB header
    writeLE32(p, kMagic);
    p += 4;
    writeLE32(p, 2u);
    p += 4;
    writeLE32(p, static_cast<uint32_t>(totalSize));
    p += 4;

    // JSON chunk
    writeLE32(p, static_cast<uint32_t>(json.size()));
    p += 4;
    writeLE32(p, kChunkJSON);
    p += 4;
    std::memcpy(p, json.data(), json.size());
    p += json.size();

    // BIN chunk
    writeLE32(p, static_cast<uint32_t>(binPadded));
    p += 4;
    writeLE32(p, kChunkBIN);
    p += 4;
    std::memcpy(p, bin.data(), binPadded);

    return glb;
}

} // namespace fl
