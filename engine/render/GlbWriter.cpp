// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/GlbWriter.h"

#include "net/ByteOrder.h" // the ONE little-endian codec (#1255)

#include <cstring>

namespace fl {

namespace {

// glTF component types. Both callers emit float attributes and unsigned-short indices; anything
// else would need a componentType per attribute, which nothing asks for yet.
constexpr int kComponentFloat = 5126;
constexpr int kComponentUnsignedShort = 5123;
constexpr int kTargetArrayBuffer = 34962;
constexpr int kTargetElementArrayBuffer = 34963;

// GLB container constants (glTF 2.0 binary).
constexpr uint32_t kMagic = 0x46546C67u;     // "glTF"
constexpr uint32_t kChunkJSON = 0x4E4F534Au; // "JSON"
constexpr uint32_t kChunkBIN = 0x004E4942u;  // "BIN\0"

std::string num(double v) {
    return std::to_string(v);
}

} // namespace

std::vector<uint8_t> buildGlb(const GlbMesh& mesh) {
    // ── BIN chunk: attributes in order, then indices. Every float/u16 block is a multiple of 4 or 2
    // bytes and the whole chunk is padded to 4, so bufferView offsets stay aligned.
    const std::size_t idxBytes = mesh.indexCount * sizeof(uint16_t);
    std::size_t binBytes = idxBytes;
    for (const GlbAttribute& a : mesh.attributes)
        binBytes += a.bytes;
    const std::size_t binPadded = (binBytes + 3u) & ~std::size_t{3u};

    std::vector<uint8_t> bin(binPadded, 0);
    std::vector<std::size_t> offsets;
    offsets.reserve(mesh.attributes.size());
    std::size_t off = 0;
    for (const GlbAttribute& a : mesh.attributes) {
        offsets.push_back(off);
        if (a.bytes)
            std::memcpy(bin.data() + off, a.data, a.bytes);
        off += a.bytes;
    }
    const std::size_t idxOff = off;
    if (idxBytes)
        std::memcpy(bin.data() + idxOff, mesh.indices, idxBytes);

    // ── JSON chunk.
    const std::size_t attrCount = mesh.attributes.size();
    std::string attrs;
    for (std::size_t i = 0; i < attrCount; ++i) {
        if (i)
            attrs += ',';
        attrs += "\"" + std::string(mesh.attributes[i].semantic) + "\":" + std::to_string(i);
    }

    std::string json = "{";
    json += R"("asset":{"version":"2.0"},)";
    json += R"("scene":0,)";
    json += R"("scenes":[{"nodes":[0]}],)";
    json += R"("nodes":[{"mesh":0}],)";
    json += "\"meshes\":[{\"name\":\"" + std::string(mesh.meshName) + "\",\"primitives\":[{\"attributes\":{" + attrs +
            "},\"indices\":" + std::to_string(attrCount) + ",\"mode\":4}]}],";

    json += "\"accessors\":[";
    for (std::size_t i = 0; i < attrCount; ++i) {
        if (i)
            json += ',';
        json += "{\"bufferView\":" + std::to_string(i) +
                ",\"byteOffset\":0,\"componentType\":" + std::to_string(kComponentFloat) +
                ",\"count\":" + std::to_string(mesh.vertexCount) + ",\"type\":\"" + mesh.attributes[i].type + "\"";
        // The spec requires min/max on the POSITION accessor (viewers use it for bounds); the others
        // are free to omit it, and both callers always did.
        if (i == 0)
            json += ",\"min\":[" + num(mesh.posMin[0]) + "," + num(mesh.posMin[1]) + "," + num(mesh.posMin[2]) +
                    "],\"max\":[" + num(mesh.posMax[0]) + "," + num(mesh.posMax[1]) + "," + num(mesh.posMax[2]) + "]";
        json += "}";
    }
    json += ",{\"bufferView\":" + std::to_string(attrCount) +
            ",\"byteOffset\":0,\"componentType\":" + std::to_string(kComponentUnsignedShort) +
            ",\"count\":" + std::to_string(mesh.indexCount) + ",\"type\":\"SCALAR\"}],";

    json += "\"bufferViews\":[";
    for (std::size_t i = 0; i < attrCount; ++i) {
        if (i)
            json += ',';
        json += "{\"buffer\":0,\"byteOffset\":" + std::to_string(offsets[i]) +
                ",\"byteLength\":" + std::to_string(mesh.attributes[i].bytes) +
                ",\"target\":" + std::to_string(kTargetArrayBuffer) + "}";
    }
    json += ",{\"buffer\":0,\"byteOffset\":" + std::to_string(idxOff) + ",\"byteLength\":" + std::to_string(idxBytes) +
            ",\"target\":" + std::to_string(kTargetElementArrayBuffer) + "}],";

    json += "\"buffers\":[{\"byteLength\":" + std::to_string(binPadded) + "}]";
    json += "}";

    // Each chunk's payload must be 4-byte aligned; JSON pads with spaces, BIN with zeroes (above).
    while (json.size() % 4 != 0)
        json += ' ';

    // ── Container: 12-byte header, then the JSON and BIN chunks.
    const std::size_t totalSize = 12 + (8 + json.size()) + (8 + binPadded);
    std::vector<uint8_t> glb;
    glb.reserve(totalSize);
    detail::putU32LE(glb, kMagic);
    detail::putU32LE(glb, 2u); // glTF version
    detail::putU32LE(glb, static_cast<uint32_t>(totalSize));
    detail::putU32LE(glb, static_cast<uint32_t>(json.size()));
    detail::putU32LE(glb, kChunkJSON);
    glb.insert(glb.end(), json.begin(), json.end());
    detail::putU32LE(glb, static_cast<uint32_t>(binPadded));
    detail::putU32LE(glb, kChunkBIN);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

} // namespace fl
