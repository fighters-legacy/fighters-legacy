// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fl {

// Writing a single-mesh binary glTF, in one place (#1265).
//
// Two generators build runtime meshes as GLB and hand them straight to the renderer: the terrain
// tile builder and the airport runway slabs. Neither result is ever persisted, so the format is
// purely an internal handoff — and both wrote out the same 12-byte header, the same two-chunk
// framing, the same 4-byte JSON pad, the same 0x46546C67/0x4E4F534A/0x004E4942 constants, and the
// same accessor/bufferView document.
//
// They had ALREADY diverged in mechanism: one wrote its length fields with a memcpy under the
// comment "host is always LE on our targets", the other with explicit shifts. Both are correct on
// the targets that exist, which is exactly the kind of divergence that is invisible until the day it
// is not. This writer uses the shared little-endian codec (net/ByteOrder.h) so there is one answer.
//
// SCOPE, deliberately narrow to what the two callers need: one mesh, one primitive, one buffer,
// non-interleaved float attributes and u16 indices. A caller wanting interleaved data or a second
// primitive should extend this rather than grow a third writer.

// One non-interleaved vertex attribute. `bytes` is already in its final layout — this writer copies
// it into the BIN chunk and describes it, it does not convert anything.
struct GlbAttribute {
    const char* semantic{nullptr}; // "POSITION", "NORMAL", "TEXCOORD_0", "TANGENT"
    const char* type{nullptr};     // glTF accessor type: "VEC2", "VEC3", "VEC4"
    const void* data{nullptr};
    std::size_t bytes{0};
};

// A single-primitive mesh. Attributes are emitted in the order given, and their accessor and
// bufferView indices follow that order with the index accessor last — which is what both callers
// already relied on.
//
// POSITION's min/max is REQUIRED by the glTF spec for the position accessor, and both callers supply
// it; it is a plain array here rather than a glm type so this header stays dependency-free.
struct GlbMesh {
    const char* meshName{"mesh"};
    uint32_t vertexCount{0};
    std::vector<GlbAttribute> attributes; // POSITION must be first
    double posMin[3]{};
    double posMax[3]{};
    const uint16_t* indices{nullptr};
    std::size_t indexCount{0};
};

[[nodiscard]] std::vector<uint8_t> buildGlb(const GlbMesh& mesh);

} // namespace fl
