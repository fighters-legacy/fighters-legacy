// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/AirportRenderer.h"

#include "IRenderer.h"

#include <cstddef>
#include <cstring>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string>

namespace fl {

namespace {

constexpr double kSlabLiftM = 0.06;     // sit a few cm above the flattened terrain
constexpr int kSegments = 12;           // tessellation along the centerline (chords stay < 1 mm sag)
constexpr double kCullRangeM = 60000.0; // draw runways within 60 km of the camera

void writeLE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

// Per-surface slab tint (baseColorFactor). Water runways emit no slab, so their entry is unused.
glm::vec4 surfaceTint(RunwaySurface s) {
    switch (s) {
    case RunwaySurface::Concrete:
        return {0.62f, 0.62f, 0.63f, 1.f};
    case RunwaySurface::Asphalt:
        return {0.22f, 0.22f, 0.24f, 1.f};
    case RunwaySurface::Grass:
        return {0.30f, 0.42f, 0.22f, 1.f};
    case RunwaySurface::Gravel:
        return {0.45f, 0.41f, 0.36f, 1.f};
    case RunwaySurface::Deck:
        return {0.35f, 0.36f, 0.40f, 1.f};
    case RunwaySurface::Water:
        return {0.10f, 0.20f, 0.30f, 1.f};
    }
    return {0.5f, 0.5f, 0.5f, 1.f};
}

} // namespace

std::vector<glm::dvec3> runwaySlabVertices(const ResolvedRunway& rw, double elevationM, double planetRadiusM) {
    std::vector<glm::dvec3> verts;
    if (rw.surface == RunwaySurface::Water)
        return verts; // the sea is the runway — no slab
    const glm::dvec3 centre{0.0, -planetRadiusM, 0.0};
    const glm::dvec3 dir0 = glm::normalize(rw.threshold - centre);
    const glm::dvec3 dir1 = glm::normalize(rw.oppositeEnd - centre);
    const double radius = planetRadiusM + elevationM + kSlabLiftM;
    const double halfW = 0.5 * static_cast<double>(rw.widthM);
    verts.reserve(static_cast<std::size_t>(kSegments + 1) * 2);
    for (int i = 0; i <= kSegments; ++i) {
        const double t = static_cast<double>(i) / kSegments;
        // Interpolate the radial direction (on the sphere) then scale to the slab radius — each
        // cross-section centre sits on the spherical datum at the field elevation + lift.
        const glm::dvec3 dir = glm::normalize(glm::mix(dir0, dir1, t));
        const glm::dvec3 centrePt = centre + dir * radius;
        const glm::dvec3 up = dir;
        const glm::dvec3 cross = glm::normalize(glm::cross(rw.centerlineDir, up)); // across the runway
        verts.push_back(centrePt - cross * halfW);                                 // left (v=0)
        verts.push_back(centrePt + cross * halfW);                                 // right (v=1)
    }
    return verts;
}

std::vector<uint8_t> buildRunwaySlabGlb(const ResolvedRunway& rw, double elevationM, double planetRadiusM,
                                        glm::dvec3 originWorld) {
    const std::vector<glm::dvec3> worldVerts = runwaySlabVertices(rw, elevationM, planetRadiusM);
    if (worldVerts.empty())
        return {};

    const int rows = kSegments + 1;
    const auto vertCount = static_cast<uint32_t>(worldVerts.size());
    std::vector<float> positions(vertCount * 3);
    std::vector<float> normals(vertCount * 3);
    std::vector<float> texcoords(vertCount * 2);
    glm::vec3 relMin{1e30f}, relMax{-1e30f};
    for (int i = 0; i < rows; ++i) {
        const glm::dvec3 dir =
            glm::normalize(worldVerts[static_cast<std::size_t>(i) * 2] - glm::dvec3{0, -planetRadiusM, 0});
        const auto up = glm::vec3(dir);
        const float u = static_cast<float>(i) / static_cast<float>(kSegments);
        for (int side = 0; side < 2; ++side) {
            const auto idx = static_cast<std::size_t>(i) * 2 + static_cast<std::size_t>(side);
            const auto rel = glm::vec3(worldVerts[idx] - originWorld);
            positions[idx * 3 + 0] = rel.x;
            positions[idx * 3 + 1] = rel.y;
            positions[idx * 3 + 2] = rel.z;
            normals[idx * 3 + 0] = up.x;
            normals[idx * 3 + 1] = up.y;
            normals[idx * 3 + 2] = up.z;
            texcoords[idx * 2 + 0] = u;
            texcoords[idx * 2 + 1] = static_cast<float>(side);
            relMin = glm::min(relMin, rel);
            relMax = glm::max(relMax, rel);
        }
    }

    std::vector<uint16_t> indices;
    indices.reserve(static_cast<std::size_t>(kSegments) * 6);
    for (int i = 0; i < kSegments; ++i) {
        const auto l0 = static_cast<uint16_t>(i * 2);
        const auto r0 = static_cast<uint16_t>(i * 2 + 1);
        const auto l1 = static_cast<uint16_t>(i * 2 + 2);
        const auto r1 = static_cast<uint16_t>(i * 2 + 3);
        // CCW-from-outside (normal = radial up): (l0, r0, r1) then (l0, r1, l1).
        indices.push_back(l0);
        indices.push_back(r0);
        indices.push_back(r1);
        indices.push_back(l0);
        indices.push_back(r1);
        indices.push_back(l1);
    }
    const auto indexCount = static_cast<uint32_t>(indices.size());

    const std::size_t posBytes = static_cast<std::size_t>(vertCount) * 3 * sizeof(float);
    const std::size_t nrmBytes = posBytes;
    const std::size_t texBytes = static_cast<std::size_t>(vertCount) * 2 * sizeof(float);
    const std::size_t idxBytes = static_cast<std::size_t>(indexCount) * sizeof(uint16_t);
    const std::size_t binBytes = posBytes + nrmBytes + texBytes + idxBytes;
    const std::size_t binPadded = (binBytes + 3u) & ~std::size_t{3u};
    const std::size_t posOff = 0, nrmOff = posBytes, texOff = nrmOff + nrmBytes, idxOff = texOff + texBytes;

    std::vector<uint8_t> bin(binPadded, 0);
    std::memcpy(bin.data() + posOff, positions.data(), posBytes);
    std::memcpy(bin.data() + nrmOff, normals.data(), nrmBytes);
    std::memcpy(bin.data() + texOff, texcoords.data(), texBytes);
    std::memcpy(bin.data() + idxOff, indices.data(), idxBytes);

    std::string json = "{";
    json += R"("asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)";
    json +=
        R"("meshes":[{"name":"runway","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"mode":4}]}],)";
    json += "\"accessors\":[";
    json += "{\"bufferView\":0,\"byteOffset\":0,\"componentType\":5126,\"count\":" + std::to_string(vertCount) +
            ",\"type\":\"VEC3\",\"min\":[" + std::to_string(relMin.x) + "," + std::to_string(relMin.y) + "," +
            std::to_string(relMin.z) + "],\"max\":[" + std::to_string(relMax.x) + "," + std::to_string(relMax.y) + "," +
            std::to_string(relMax.z) + "]},";
    json += "{\"bufferView\":1,\"byteOffset\":0,\"componentType\":5126,\"count\":" + std::to_string(vertCount) +
            ",\"type\":\"VEC3\"},";
    json += "{\"bufferView\":2,\"byteOffset\":0,\"componentType\":5126,\"count\":" + std::to_string(vertCount) +
            ",\"type\":\"VEC2\"},";
    json += "{\"bufferView\":3,\"byteOffset\":0,\"componentType\":5123,\"count\":" + std::to_string(indexCount) +
            ",\"type\":\"SCALAR\"}],";
    json += "\"bufferViews\":[";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(posOff) + ",\"byteLength\":" + std::to_string(posBytes) +
            ",\"target\":34962},";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(nrmOff) + ",\"byteLength\":" + std::to_string(nrmBytes) +
            ",\"target\":34962},";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(texOff) + ",\"byteLength\":" + std::to_string(texBytes) +
            ",\"target\":34962},";
    json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(idxOff) + ",\"byteLength\":" + std::to_string(idxBytes) +
            ",\"target\":34963}],";
    json += "\"buffers\":[{\"byteLength\":" + std::to_string(binPadded) + "}]}";
    while (json.size() % 4 != 0)
        json += ' ';

    static constexpr uint32_t kChunkJSON = 0x4E4F534Au, kChunkBIN = 0x004E4942u, kMagic = 0x46546C67u;
    const std::size_t totalSize = 12 + (8 + json.size()) + (8 + binPadded);
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

AirportRenderer::AirportRenderer(IRenderer& renderer) : m_renderer(renderer) {}

void AirportRenderer::ensureMaterials() {
    if (m_materialsBuilt)
        return;
    for (int i = 0; i < 6; ++i) {
        MaterialDesc md{};
        md.baseColorFactor = surfaceTint(static_cast<RunwaySurface>(i));
        md.roughnessFactor = 0.95f;
        md.metallicFactor = 0.f;
        m_materials[i] = m_renderer.createMaterial(md);
    }
    m_materialsBuilt = true;
}

void AirportRenderer::appendRenderItems(glm::dvec3 cameraWorldOrigin, std::vector<RenderItem>& out) {
    if (!m_registry)
        return;
    ensureMaterials();
    const double R = m_registry->planetRadiusM();

    // Cull to airports near the camera via the registry grid.
    const LatLonAlt camLla = worldToGeodetic(cameraWorldOrigin.x, cameraWorldOrigin.y, cameraWorldOrigin.z, R);
    std::vector<const ResolvedAirport*> near;
    m_registry->airportsNear(camLla, kCullRangeM / 1000.0, near);

    for (const ResolvedAirport* a : near) {
        for (std::size_t r = 0; r < a->runways.size(); ++r) {
            const ResolvedRunway& rw = a->runways[r];
            if (rw.surface == RunwaySurface::Water)
                continue;
            const std::string key = a->def.id + ":" + std::to_string(r);
            auto it = m_cache.find(key);
            if (it == m_cache.end()) {
                const glm::dvec3 origin = 0.5 * (rw.threshold + rw.oppositeEnd);
                auto glb = buildRunwaySlabGlb(rw, a->elevationM, R, origin);
                if (glb.empty())
                    continue;
                CachedRunway cr;
                cr.origin = origin;
                cr.paved = (rw.surface == RunwaySurface::Concrete || rw.surface == RunwaySurface::Asphalt);
                cr.mesh = m_renderer.createMesh({"runway:" + key, glb});
                it = m_cache.emplace(key, cr).first;
            }
            const CachedRunway& cr = it->second;
            if (!cr.mesh.valid())
                continue;
            RenderItem item{};
            item.mesh = cr.mesh;
            item.material = m_materials[static_cast<int>(rw.surface)];
            item.transform = glm::translate(glm::mat4(1.0f), glm::vec3(cr.origin - cameraWorldOrigin));
            if (cr.paved)
                item.flags |= kRenderFlagRunway; // paved → procedural markings in the shader
            out.push_back(item);
        }
    }
}

} // namespace fl
