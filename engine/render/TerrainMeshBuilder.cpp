// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/TerrainMeshBuilder.h"

#include <glm/common.hpp>    // min, max (component-wise)
#include <glm/geometric.hpp> // cross, normalize, dot, length

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
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

std::vector<uint8_t> buildTileMeshGlb(const std::vector<uint16_t>& heights, int heightmapSize, int meshGrid,
                                      const TileKey& key, double R, glm::dvec3 tileOriginWorld,
                                      const uint8_t* landCover, bool emitTexcoord, double skirtDepthM) noexcept {
    // Validate input.
    if (heights.empty() || heightmapSize < 2 || meshGrid <= 0)
        return {};
    if (heightmapSize < meshGrid + 1)
        return {};
    if (static_cast<int>(heights.size()) < heightmapSize * heightmapSize)
        return {};

    const int stride = (heightmapSize - 1) / meshGrid; // integer, >= 1
    const int gridPts = meshGrid + 1;                  // vertices per side
    const int baseVertCount = gridPts * gridPts;
    const bool hasSkirt = skirtDepthM > 0.0;
    const int skirtVertCount = hasSkirt ? 4 * gridPts : 0;
    const int vertCount = baseVertCount + skirtVertCount;
    if (vertCount > static_cast<int>(std::numeric_limits<uint16_t>::max()) + 1)
        return {}; // uint16 index budget exceeded
    const int quadCount = meshGrid * meshGrid;
    const int indexCount = quadCount * 6 + (hasSkirt ? 4 * meshGrid * 6 : 0);
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

    // The packed terrain "tangent" (VEC4 f32) rides with TEXCOORD_0 — terrain does not use a real
    // tangent (it shades from the geometric normal + screen-space derivatives), so this attribute
    // carries the per-vertex biome/detail data instead (#475): .x = WorldCover class (0..11, or 255 =
    // "no land-cover, use elevation/slope fallback"), .y = normalized elevation ((h+1000)/10000
    // clamped, for the snow line), .zw = spherical-valid detail coordinate in metres — the global
    // face-UV arc length reduced modulo kDetailPeriod. Because coincident vertices on adjacent tiles
    // (and across LOD levels) share the exact same face-UV, the coordinate is continuous by
    // construction; kDetailPeriod is a common multiple of the shader's fine (4 m) and coarse (30 m)
    // detail tiling, so the modulo wrap is seamless too. Only the 8 cube-face seams differ (accepted).
    const bool emitTerrainTangent = emitTexcoord;
    const double kFaceArc = R * (std::numbers::pi / 2.0); // ~1 face edge arc length (90 deg)
    const double kDetailPeriod = 3000.0;                  // = 4 m x 750 = 30 m x 100 (seamless wrap)
    const double nLevel = static_cast<double>(uint64_t{1} << key.level);

    std::vector<float> positions(static_cast<std::size_t>(vertCount) * 3);
    std::vector<float> normals(static_cast<std::size_t>(vertCount) * 3);
    std::vector<float> texcoords; // VEC2
    std::vector<float> tangents;  // VEC4 f32 (packed terrain data, #475)
    if (emitTexcoord)
        texcoords.resize(static_cast<std::size_t>(vertCount) * 2);
    if (emitTerrainTangent)
        tangents.resize(static_cast<std::size_t>(vertCount) * 4);

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
            if (emitTerrainTangent) {
                // WorldCover class (255 = none -> shader falls back to elevation/slope selection).
                float cls = 255.0f;
                if (landCover) {
                    const int pc = std::clamp(col * stride, 0, heightmapSize - 1);
                    const int pr = std::clamp(row * stride, 0, heightmapSize - 1);
                    cls = static_cast<float>(landCover[static_cast<std::size_t>(pr) * heightmapSize + pc]);
                }
                const double h = hAt(col * stride, row * stride);
                const float normElev = static_cast<float>(std::clamp((h + 1000.0) / 10000.0, 0.0, 1.0));
                const double faceU = (static_cast<double>(key.i) + static_cast<double>(col) / n) / nLevel;
                const double faceV = (static_cast<double>(key.j) + static_cast<double>(row) / n) / nLevel;
                tangents[vi * 4 + 0] = cls;
                tangents[vi * 4 + 1] = normElev;
                tangents[vi * 4 + 2] = static_cast<float>(std::fmod(faceU * kFaceArc, kDetailPeriod));
                tangents[vi * 4 + 3] = static_cast<float>(std::fmod(faceV * kFaceArc, kDetailPeriod));
            }
        }
    }

    // Indices. CCW-from-outside: the tile basis (col->U, row->V, out=U x V) is right-handed, so
    // triangles wind (v, vdr, vd) / (v, vr, vdr) to keep the face normal outward (validate-mesh clean).
    std::vector<uint16_t> indices(static_cast<std::size_t>(indexCount));
    std::size_t ii = 0;
    for (int row = 0; row < meshGrid; ++row) {
        for (int col = 0; col < meshGrid; ++col) {
            const uint16_t v = static_cast<uint16_t>(row * gridPts + col);
            const uint16_t vr = static_cast<uint16_t>(v + 1);
            const uint16_t vd = static_cast<uint16_t>(v + gridPts);
            const uint16_t vdr = static_cast<uint16_t>(v + gridPts + 1);
            indices[ii++] = v;
            indices[ii++] = vdr;
            indices[ii++] = vd;
            indices[ii++] = v;
            indices[ii++] = vr;
            indices[ii++] = vdr;
        }
    }

    // Skirt (#472): duplicate each border vertex, push it toward the planet centre by skirtDepthM,
    // and seal the border with side quads. Copies of normal/texcoord/color make the flap shade like
    // the surface, so the crack it hides is invisible rather than a dark seam.
    if (hasSkirt) {
        // Border vertex source indices, one run of gridPts per edge: -V row, +V row, -U col, +U col.
        std::vector<uint16_t> edgeSrc(static_cast<std::size_t>(4) * gridPts);
        for (int k = 0; k < gridPts; ++k) {
            edgeSrc[static_cast<std::size_t>(0) * gridPts + k] = static_cast<uint16_t>(k); // row 0
            edgeSrc[static_cast<std::size_t>(1) * gridPts + k] =
                static_cast<uint16_t>(meshGrid * gridPts + k);                                       // row meshGrid
            edgeSrc[static_cast<std::size_t>(2) * gridPts + k] = static_cast<uint16_t>(k * gridPts); // col 0
            edgeSrc[static_cast<std::size_t>(3) * gridPts + k] =
                static_cast<uint16_t>(k * gridPts + meshGrid); // col meshGrid
        }

        const glm::dvec3 tileCentre = tileToWorld(key, 0.5, 0.5, 0.0, R);
        for (int e = 0; e < 4; ++e) {
            const int firstSkirt = baseVertCount + e * gridPts;
            for (int k = 0; k < gridPts; ++k) {
                const std::size_t src = edgeSrc[static_cast<std::size_t>(e) * gridPts + k];
                const std::size_t dst = static_cast<std::size_t>(firstSkirt + k);
                const glm::dvec3 world{static_cast<double>(positions[src * 3 + 0]) + tileOriginWorld.x,
                                       static_cast<double>(positions[src * 3 + 1]) + tileOriginWorld.y,
                                       static_cast<double>(positions[src * 3 + 2]) + tileOriginWorld.z};
                const glm::dvec3 inward = glm::normalize(centre - world);
                const glm::vec3 rel = glm::vec3(world + inward * skirtDepthM - tileOriginWorld);
                positions[dst * 3 + 0] = rel.x;
                positions[dst * 3 + 1] = rel.y;
                positions[dst * 3 + 2] = rel.z;
                relMin = glm::min(relMin, rel);
                relMax = glm::max(relMax, rel);
                normals[dst * 3 + 0] = normals[src * 3 + 0];
                normals[dst * 3 + 1] = normals[src * 3 + 1];
                normals[dst * 3 + 2] = normals[src * 3 + 2];
                if (emitTexcoord) {
                    texcoords[dst * 2 + 0] = texcoords[src * 2 + 0];
                    texcoords[dst * 2 + 1] = texcoords[src * 2 + 1];
                }
                if (emitTerrainTangent) {
                    for (int c = 0; c < 4; ++c)
                        tangents[dst * 4 + c] = tangents[src * 4 + c];
                }
            }

            // Winding per edge: pick the triangulation whose face normal points away from the tile
            // centre (front-facing from outside the tile border under back-face culling).
            auto relAt = [&](std::size_t idx) -> glm::dvec3 {
                return {static_cast<double>(positions[idx * 3 + 0]), static_cast<double>(positions[idx * 3 + 1]),
                        static_cast<double>(positions[idx * 3 + 2])};
            };
            const std::size_t a0 = edgeSrc[static_cast<std::size_t>(e) * gridPts];
            const std::size_t b0 = edgeSrc[static_cast<std::size_t>(e) * gridPts + 1];
            const glm::dvec3 pa = relAt(a0), pb = relAt(b0), pb2 = relAt(static_cast<std::size_t>(firstSkirt + 1));
            const glm::dvec3 edgeOut = (pa + pb) * 0.5 - (tileCentre - tileOriginWorld);
            const bool forward = glm::dot(glm::cross(pb - pa, pb2 - pa), edgeOut) >= 0.0;

            for (int k = 0; k < meshGrid; ++k) {
                const uint16_t a = edgeSrc[static_cast<std::size_t>(e) * gridPts + k];
                const uint16_t b = edgeSrc[static_cast<std::size_t>(e) * gridPts + k + 1];
                const uint16_t a2 = static_cast<uint16_t>(firstSkirt + k);
                const uint16_t b2 = static_cast<uint16_t>(firstSkirt + k + 1);
                if (forward) {
                    indices[ii++] = a;
                    indices[ii++] = b;
                    indices[ii++] = b2;
                    indices[ii++] = a;
                    indices[ii++] = b2;
                    indices[ii++] = a2;
                } else {
                    indices[ii++] = a;
                    indices[ii++] = b2;
                    indices[ii++] = b;
                    indices[ii++] = a;
                    indices[ii++] = a2;
                    indices[ii++] = b2;
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Assemble BIN buffer (non-interleaved): POSITION, NORMAL, [TEXCOORD_0], [TANGENT], INDICES.
    // TANGENT (VEC4 f32) carries the packed terrain data (#475), not a real tangent — see the fill
    // loop. All attribute blocks are multiples of 4 bytes, so bufferView offsets stay 4-aligned.
    // ---------------------------------------------------------------------------
    const std::size_t posBytes = static_cast<std::size_t>(vertCount) * 3 * sizeof(float);
    const std::size_t nrmBytes = posBytes;
    const std::size_t texBytes = emitTexcoord ? static_cast<std::size_t>(vertCount) * 2 * sizeof(float) : 0;
    const std::size_t tanBytes = emitTerrainTangent ? static_cast<std::size_t>(vertCount) * 4 * sizeof(float) : 0;
    const std::size_t idxBytes = static_cast<std::size_t>(indexCount) * sizeof(uint16_t);
    const std::size_t binBytes = posBytes + nrmBytes + texBytes + tanBytes + idxBytes;
    const std::size_t binPadded = (binBytes + 3u) & ~std::size_t{3u};

    const std::size_t posOff = 0;
    const std::size_t nrmOff = posOff + posBytes;
    const std::size_t texOff = nrmOff + nrmBytes;
    const std::size_t tanOff = texOff + texBytes;
    const std::size_t idxOff = tanOff + tanBytes;

    std::vector<uint8_t> bin(binPadded, 0);
    std::memcpy(bin.data() + posOff, positions.data(), posBytes);
    std::memcpy(bin.data() + nrmOff, normals.data(), nrmBytes);
    if (texBytes)
        std::memcpy(bin.data() + texOff, texcoords.data(), texBytes);
    if (tanBytes)
        std::memcpy(bin.data() + tanOff, tangents.data(), tanBytes);
    std::memcpy(bin.data() + idxOff, indices.data(), idxBytes);

    // ---------------------------------------------------------------------------
    // Build JSON (glTF 2.0) with dynamically ordered accessors / bufferViews.
    // Accessor order: 0=POSITION, 1=NORMAL, [TEXCOORD_0], [TANGENT], last=INDICES.
    // ---------------------------------------------------------------------------
    int accPos = 0, accNrm = 1, accNext = 2;
    int accTex = -1, accTan = -1;
    if (emitTexcoord)
        accTex = accNext++;
    if (emitTerrainTangent)
        accTan = accNext++;
    const int accIdx = accNext;
    const int bvTex = emitTexcoord ? 2 : -1;
    const int bvTan = emitTerrainTangent ? (emitTexcoord ? 3 : 2) : -1;
    const int bvIdx = accIdx;

    std::string attrs = "\"POSITION\":" + std::to_string(accPos) + ",\"NORMAL\":" + std::to_string(accNrm);
    if (emitTexcoord)
        attrs += ",\"TEXCOORD_0\":" + std::to_string(accTex);
    if (emitTerrainTangent)
        attrs += ",\"TANGENT\":" + std::to_string(accTan);

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
    if (emitTerrainTangent)
        json += ",{\"bufferView\":" + std::to_string(bvTan) +
                ",\"byteOffset\":0,\"componentType\":5126,\"count\":" + std::to_string(vertCount) +
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
    if (emitTerrainTangent)
        json += ",{\"buffer\":0,\"byteOffset\":" + std::to_string(tanOff) +
                ",\"byteLength\":" + std::to_string(tanBytes) + ",\"target\":34962}";
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
