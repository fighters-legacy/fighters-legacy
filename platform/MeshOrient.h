// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <glm/vec3.hpp>

namespace fl {

// Content → body axis mapping (#906).
//
// The engine's body frame is aerospace convention: +X forward (nose), +Y up, +Z starboard. That is
// NOT the frame a modeller works in: a standard Blender glTF export puts the model's nose along the
// glTF content-forward axis, +Z. Requiring authors to build "wrong" (nose along +X) — or hand-correct
// on export — is a papercut that already shipped both fl-base-pack aircraft flying tail-first.
//
// So a content mesh (flagged MeshUploadDesc::contentForward) is authored in the standard glTF
// convention and rotated into the body frame on upload: a fixed +90° about the +Y (up) axis, which
// maps content-forward +Z → body-forward +X (and content +X → body −Z, i.e. content-left stays left).
// Ry(+90°) = [0 0 1; 0 1 0; −1 0 0], so (x, y, z) → (z, y, −x). Up (+Y) is unchanged. It is a proper
// rotation (det +1), so triangle winding and outward-normal agreement are preserved — no winding flip.
//
// The flight model's internal body axes are untouched; this is purely the content→engine orientation
// mapping. Apply it to positions, normals, and tangents alike (a normal is a direction; the same
// rotation is correct because there is no non-uniform scale). Engine-generated meshes (terrain tiles,
// the builtin placeholders and floor) are already in the body/world frame and leave the flag false.
[[nodiscard]] inline glm::vec3 contentForwardToBody(const glm::vec3& v) noexcept {
    return {v.z, v.y, -v.x};
}

} // namespace fl
