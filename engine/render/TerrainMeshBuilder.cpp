// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/TerrainMeshBuilder.h"

#include <glm/common.hpp>    // min, max (component-wise)
#include <glm/geometric.hpp> // cross, normalize, dot, length

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
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
                                         float chunkSizeM, double chunkWorldX, double chunkWorldZ,
                                         double planetRadius) noexcept {
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

    // World-space distance per heightmap pixel (for spherical correction lookup)
    const double hPixelToWorld = static_cast<double>(chunkSizeM) / static_cast<double>(heightmapSize - 1);
    const double R2 = planetRadius * planetRadius;

    // height lookup (clamped)
    auto hAt = [&](int col, int row) -> float {
        col = std::clamp(col, 0, heightmapSize - 1);
        row = std::clamp(row, 0, heightmapSize - 1);
        return static_cast<float>(heights[static_cast<std::size_t>(row) * heightmapSize + col]) - 32768.0f;
    };

    // Height + spherical correction at heightmap pixel (hcIn, hrIn).
    // hcIn/hrIn are intentionally unclamped for world-position lookup; hAt() clamps internally.
    auto hAtSphere = [&](int hcIn, int hrIn) -> float {
        const float base = hAt(hcIn, hrIn);
        if (planetRadius <= 0.0)
            return base;
        const double vx = chunkWorldX + hcIn * hPixelToWorld;
        const double vz = chunkWorldZ + hrIn * hPixelToWorld;
        const double D2 = vx * vx + vz * vz;
        return base + static_cast<float>(std::sqrt(std::max(0.0, R2 - D2)) - planetRadius);
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
            const float y = hAtSphere(hc, hr);

            const std::size_t vi = static_cast<std::size_t>(row * gridPts + col);
            positions[vi * 3 + 0] = static_cast<float>(col) * cellM;
            positions[vi * 3 + 1] = y;
            positions[vi * 3 + 2] = static_cast<float>(row) * cellM;

            if (y < yMin)
                yMin = y;
            if (y > yMax)
                yMax = y;

            // Central-difference normal (uses spherical-corrected Y to account for curvature gradient)
            const float hl = hAtSphere(hc - stride, hr);
            const float hr_ = hAtSphere(hc + stride, hr);
            const float hu = hAtSphere(hc, hr - stride);
            const float hd = hAtSphere(hc, hr + stride);
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

    // Build index array. Standard glTF winding: CCW-from-above so the winding cross-product
    // agrees with the +Y stored normal (front-faces the top surface under frontFace=CCW).
    std::vector<uint16_t> indices(static_cast<std::size_t>(indexCount));
    std::size_t ii = 0;
    for (int row = 0; row < meshGrid; ++row) {
        for (int col = 0; col < meshGrid; ++col) {
            const uint16_t v = static_cast<uint16_t>(row * gridPts + col);
            const uint16_t vr = static_cast<uint16_t>(v + 1);
            const uint16_t vd = static_cast<uint16_t>(v + gridPts);
            const uint16_t vdr = static_cast<uint16_t>(v + gridPts + 1);
            // tri 0: v, vd, vdr
            indices[ii++] = v;
            indices[ii++] = vd;
            indices[ii++] = vdr;
            // tri 1: v, vdr, vr
            indices[ii++] = v;
            indices[ii++] = vdr;
            indices[ii++] = vr;
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

std::vector<uint8_t> buildTileMeshGlb(const std::vector<uint16_t>& heights, int heightmapSize, int meshGrid,
                                      const TileKey& key, double R, glm::dvec3 tileOriginWorld,
                                      const uint8_t* landCover, bool emitTexcoord) noexcept {
    // Validate input (mirrors buildTerrainMeshGlb).
    if (heights.empty() || heightmapSize < 2 || meshGrid <= 0)
        return {};
    if (heightmapSize < meshGrid + 1)
        return {};
    if (static_cast<int>(heights.size()) < heightmapSize * heightmapSize)
        return {};

    const int stride = (heightmapSize - 1) / meshGrid; // integer, >= 1
    const int gridPts = meshGrid + 1;                  // vertices per side
    const int vertCount = gridPts * gridPts;
    const int quadCount = meshGrid * meshGrid;
    const int indexCount = quadCount * 6;
    const double n = static_cast<double>(meshGrid);
    const glm::dvec3 centre{0.0, -R, 0.0};

    // Clamped elevation lookup (metres above the sphere) at a heightmap pixel.
    auto hAt = [&](int col, int row) -> double {
        col = std::clamp(col, 0, heightmapSize - 1);
        row = std::clamp(row, 0, heightmapSize - 1);
        return static_cast<double>(heights[static_cast<std::size_t>(row) * heightmapSize + col]) - 32768.0;
    };
    // World position of the mesh vertex at grid (ic, ir), clamped to the tile so edge normals use a
    // one-sided difference rather than reading past the tile.
    auto worldAtGrid = [&](int ic, int ir) -> glm::dvec3 {
        ic = std::clamp(ic, 0, meshGrid);
        ir = std::clamp(ir, 0, meshGrid);
        const double s = static_cast<double>(ic) / n;
        const double t = static_cast<double>(ir) / n;
        const double h = hAt(ic * stride, ir * stride);
        return tileToWorld(key, s, t, h, R);
    };

    std::vector<float> positions(static_cast<std::size_t>(vertCount) * 3);
    std::vector<float> normals(static_cast<std::size_t>(vertCount) * 3);
    std::vector<float> texcoords; // VEC2
    std::vector<uint8_t> colors;  // VEC4 u8
    if (emitTexcoord)
        texcoords.resize(static_cast<std::size_t>(vertCount) * 2);
    if (landCover)
        colors.resize(static_cast<std::size_t>(vertCount) * 4);

    glm::vec3 relMin(std::numeric_limits<float>::max());
    glm::vec3 relMax(-std::numeric_limits<float>::max());

    for (int row = 0; row < gridPts; ++row) {
        for (int col = 0; col < gridPts; ++col) {
            const std::size_t vi = static_cast<std::size_t>(row * gridPts + col);
            const glm::dvec3 world = worldAtGrid(col, row);
            const glm::vec3 rel = glm::vec3(world - tileOriginWorld);

            positions[vi * 3 + 0] = rel.x;
            positions[vi * 3 + 1] = rel.y;
            positions[vi * 3 + 2] = rel.z;
            relMin = glm::min(relMin, rel);
            relMax = glm::max(relMax, rel);

            // True surface normal from the two central-difference surface tangents (captures both the
            // sphere curvature and the terrain slope). cross(dP/ds, dP/dt) points outward because the
            // (s->U, t->V) tile basis is right-handed with outward normal U x V.
            const glm::dvec3 tanS = worldAtGrid(col + 1, row) - worldAtGrid(col - 1, row);
            const glm::dvec3 tanT = worldAtGrid(col, row + 1) - worldAtGrid(col, row - 1);
            glm::dvec3 nrm = glm::cross(tanS, tanT);
            const double nlen = glm::length(nrm);
            const glm::dvec3 outward = glm::normalize(world - centre);
            if (nlen > 0.0) {
                nrm /= nlen;
                if (glm::dot(nrm, outward) < 0.0)
                    nrm = -nrm; // safety: keep normals outward
            } else {
                nrm = outward; // degenerate tangents (should not happen) -> radial up
            }
            normals[vi * 3 + 0] = static_cast<float>(nrm.x);
            normals[vi * 3 + 1] = static_cast<float>(nrm.y);
            normals[vi * 3 + 2] = static_cast<float>(nrm.z);

            if (emitTexcoord) {
                texcoords[vi * 2 + 0] = static_cast<float>(col) / static_cast<float>(meshGrid);
                texcoords[vi * 2 + 1] = static_cast<float>(row) / static_cast<float>(meshGrid);
            }
            if (landCover) {
                const int pc = std::clamp(col * stride, 0, heightmapSize - 1);
                const int pr = std::clamp(row * stride, 0, heightmapSize - 1);
                const uint8_t cls = landCover[static_cast<std::size_t>(pr) * heightmapSize + pc];
                colors[vi * 4 + 0] = cls; // WorldCover class in .r (normalized u8)
                colors[vi * 4 + 1] = 0;
                colors[vi * 4 + 2] = 0;
                colors[vi * 4 + 3] = 255;
            }
        }
    }

    // Indices. CCW-from-outside: the tile basis (col->U, row->V, out=U x V) is right-handed, the
    // opposite handedness to the planar builder's (col->X, row->Z, up->+Y), so the winding is reversed
    // relative to buildTerrainMeshGlb to keep the face normal outward (validate-mesh clean).
    std::vector<uint16_t> indices(static_cast<std::size_t>(indexCount));
    std::size_t ii = 0;
    for (int row = 0; row < meshGrid; ++row) {
        for (int col = 0; col < meshGrid; ++col) {
            const uint16_t v = static_cast<uint16_t>(row * gridPts + col);
            const uint16_t vr = static_cast<uint16_t>(v + 1);
            const uint16_t vd = static_cast<uint16_t>(v + gridPts);
            const uint16_t vdr = static_cast<uint16_t>(v + gridPts + 1);
            // tri 0: v, vdr, vd   tri 1: v, vr, vdr  (reversed vs the planar grid)
            indices[ii++] = v;
            indices[ii++] = vdr;
            indices[ii++] = vd;
            indices[ii++] = v;
            indices[ii++] = vr;
            indices[ii++] = vdr;
        }
    }

    // ---------------------------------------------------------------------------
    // Assemble BIN buffer (non-interleaved): POSITION, NORMAL, [TEXCOORD_0], [COLOR_0], INDICES.
    // All attribute blocks are multiples of 4 bytes, so bufferView offsets stay 4-aligned.
    // ---------------------------------------------------------------------------
    const std::size_t posBytes = static_cast<std::size_t>(vertCount) * 3 * sizeof(float);
    const std::size_t nrmBytes = posBytes;
    const std::size_t texBytes = emitTexcoord ? static_cast<std::size_t>(vertCount) * 2 * sizeof(float) : 0;
    const std::size_t colBytes = landCover ? static_cast<std::size_t>(vertCount) * 4 : 0;
    const std::size_t idxBytes = static_cast<std::size_t>(indexCount) * sizeof(uint16_t);
    const std::size_t binBytes = posBytes + nrmBytes + texBytes + colBytes + idxBytes;
    const std::size_t binPadded = (binBytes + 3u) & ~std::size_t{3u};

    const std::size_t posOff = 0;
    const std::size_t nrmOff = posOff + posBytes;
    const std::size_t texOff = nrmOff + nrmBytes;
    const std::size_t colOff = texOff + texBytes;
    const std::size_t idxOff = colOff + colBytes;

    std::vector<uint8_t> bin(binPadded, 0);
    std::memcpy(bin.data() + posOff, positions.data(), posBytes);
    std::memcpy(bin.data() + nrmOff, normals.data(), nrmBytes);
    if (texBytes)
        std::memcpy(bin.data() + texOff, texcoords.data(), texBytes);
    if (colBytes)
        std::memcpy(bin.data() + colOff, colors.data(), colBytes);
    std::memcpy(bin.data() + idxOff, indices.data(), idxBytes);

    // ---------------------------------------------------------------------------
    // Build JSON (glTF 2.0) with dynamically ordered accessors / bufferViews.
    // Accessor order: 0=POSITION, 1=NORMAL, [TEXCOORD_0], [COLOR_0], last=INDICES.
    // ---------------------------------------------------------------------------
    int accPos = 0, accNrm = 1, accNext = 2;
    int accTex = -1, accCol = -1;
    int bvNext = 2; // bv0=POSITION, bv1=NORMAL
    if (emitTexcoord) {
        accTex = accNext++;
        ++bvNext;
    }
    if (landCover) {
        accCol = accNext++;
        ++bvNext;
    }
    const int accIdx = accNext;
    const int bvIdx = bvNext;
    const int bvTex = emitTexcoord ? 2 : -1;
    const int bvCol = landCover ? (emitTexcoord ? 3 : 2) : -1;

    std::string attrs = "\"POSITION\":" + std::to_string(accPos) + ",\"NORMAL\":" + std::to_string(accNrm);
    if (emitTexcoord)
        attrs += ",\"TEXCOORD_0\":" + std::to_string(accTex);
    if (landCover)
        attrs += ",\"COLOR_0\":" + std::to_string(accCol);

    std::string json = "{";
    json += R"("asset":{"version":"2.0"},)";
    json += R"("scene":0,)";
    json += R"("scenes":[{"nodes":[0]}],)";
    json += R"("nodes":[{"mesh":0}],)";
    json += "\"meshes\":[{\"name\":\"tile\",\"primitives\":[{\"attributes\":{" + attrs +
            "},\"indices\":" + std::to_string(accIdx) + ",\"mode\":4}]}],";

    // accessors
    json += "\"accessors\":[";
    json += "{\"bufferView\":0,\"byteOffset\":0,\"componentType\":5126,\"count\":" + std::to_string(vertCount) +
            ",\"type\":\"VEC3\",\"min\":[" + std::to_string(relMin.x) + "," + std::to_string(relMin.y) + "," +
            std::to_string(relMin.z) + "],\"max\":[" + std::to_string(relMax.x) + "," + std::to_string(relMax.y) + "," +
            std::to_string(relMax.z) + "]},";
    json += "{\"bufferView\":1,\"byteOffset\":0,\"componentType\":5126,\"count\":" + std::to_string(vertCount) +
            ",\"type\":\"VEC3\"}";
    if (emitTexcoord)
        json += ",{\"bufferView\":" + std::to_string(bvTex) +
                ",\"byteOffset\":0,\"componentType\":5126,\"count\":" + std::to_string(vertCount) +
                ",\"type\":\"VEC2\"}";
    if (landCover)
        json += ",{\"bufferView\":" + std::to_string(bvCol) +
                ",\"byteOffset\":0,\"componentType\":5121,\"normalized\":true,\"count\":" + std::to_string(vertCount) +
                ",\"type\":\"VEC4\"}";
    json += ",{\"bufferView\":" + std::to_string(bvIdx) +
            ",\"byteOffset\":0,\"componentType\":5123,\"count\":" + std::to_string(indexCount) +
            ",\"type\":\"SCALAR\"}";
    json += "],";

    // bufferViews (same order as accessors, indices last)
    json += "\"bufferViews\":[";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(posOff) + ",\"byteLength\":" + std::to_string(posBytes) +
            ",\"target\":34962},";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(nrmOff) + ",\"byteLength\":" + std::to_string(nrmBytes) +
            ",\"target\":34962}";
    if (emitTexcoord)
        json += ",{\"buffer\":0,\"byteOffset\":" + std::to_string(texOff) +
                ",\"byteLength\":" + std::to_string(texBytes) + ",\"target\":34962}";
    if (landCover)
        json += ",{\"buffer\":0,\"byteOffset\":" + std::to_string(colOff) +
                ",\"byteLength\":" + std::to_string(colBytes) + ",\"target\":34962}";
    json += ",{\"buffer\":0,\"byteOffset\":" + std::to_string(idxOff) + ",\"byteLength\":" + std::to_string(idxBytes) +
            ",\"target\":34963}],";

    json += "\"buffers\":[{\"byteLength\":" + std::to_string(binPadded) + "}]";
    json += "}";

    while (json.size() % 4 != 0)
        json += ' ';

    static constexpr uint32_t kChunkJSON = 0x4E4F534Au;
    static constexpr uint32_t kChunkBIN = 0x004E4942u;
    static constexpr uint32_t kMagic = 0x46546C67u;

    const std::size_t jsonChunkSize = 8 + json.size();
    const std::size_t binChunkSize = 8 + binPadded;
    const std::size_t totalSize = 12 + jsonChunkSize + binChunkSize;

    std::vector<uint8_t> glb(totalSize, 0);
    uint8_t* p = glb.data();
    writeLE32(p, kMagic);
    p += 4;
    writeLE32(p, 2u);
    p += 4;
    writeLE32(p, static_cast<uint32_t>(totalSize));
    p += 4;
    writeLE32(p, static_cast<uint32_t>(json.size()));
    p += 4;
    writeLE32(p, kChunkJSON);
    p += 4;
    std::memcpy(p, json.data(), json.size());
    p += json.size();
    writeLE32(p, static_cast<uint32_t>(binPadded));
    p += 4;
    writeLE32(p, kChunkBIN);
    p += 4;
    std::memcpy(p, bin.data(), binPadded);

    return glb;
}

} // namespace fl
