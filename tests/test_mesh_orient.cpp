// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "MeshOrient.h"

using Catch::Matchers::WithinAbs;
using namespace fl;

// #906: a content mesh authored in the glTF/Blender convention (nose along +Z) is rotated into the
// engine body frame (nose along +X) on upload. The transform is a fixed +90 deg about +Y.

TEST_CASE("contentForwardToBody maps content-forward +Z to body-forward +X", "[mesh_orient]") {
    const glm::vec3 r = contentForwardToBody({0.f, 0.f, 1.f});
    CHECK_THAT(r.x, WithinAbs(1.f, 1e-6f));
    CHECK_THAT(r.y, WithinAbs(0.f, 1e-6f));
    CHECK_THAT(r.z, WithinAbs(0.f, 1e-6f));
}

TEST_CASE("contentForwardToBody keeps up (+Y) unchanged", "[mesh_orient]") {
    const glm::vec3 r = contentForwardToBody({0.f, 1.f, 0.f});
    CHECK_THAT(r.x, WithinAbs(0.f, 1e-6f));
    CHECK_THAT(r.y, WithinAbs(1.f, 1e-6f));
    CHECK_THAT(r.z, WithinAbs(0.f, 1e-6f));
}

TEST_CASE("contentForwardToBody keeps left as left (content +X -> body -Z)", "[mesh_orient]") {
    // In a +Z-forward / +Y-up right-handed frame, +X is the aircraft's LEFT; the engine's left is −Z.
    const glm::vec3 r = contentForwardToBody({1.f, 0.f, 0.f});
    CHECK_THAT(r.x, WithinAbs(0.f, 1e-6f));
    CHECK_THAT(r.z, WithinAbs(-1.f, 1e-6f));
}

TEST_CASE("contentForwardToBody is a proper rotation: preserves length and handedness", "[mesh_orient]") {
    // Length preserved (rotation, not scale).
    const glm::vec3 v{0.3f, -1.7f, 2.1f};
    const glm::vec3 r = contentForwardToBody(v);
    const float len2v = v.x * v.x + v.y * v.y + v.z * v.z;
    const float len2r = r.x * r.x + r.y * r.y + r.z * r.z;
    CHECK_THAT(len2r, WithinAbs(len2v, 1e-4f));

    // Right-handedness preserved: X̂ × Ŷ must still give Ẑ after the map (det = +1, not a reflection).
    const glm::vec3 ex = contentForwardToBody({1.f, 0.f, 0.f});
    const glm::vec3 ey = contentForwardToBody({0.f, 1.f, 0.f});
    const glm::vec3 ez = contentForwardToBody({0.f, 0.f, 1.f});
    const glm::vec3 cross{ex.y * ey.z - ex.z * ey.y, ex.z * ey.x - ex.x * ey.z, ex.x * ey.y - ex.y * ey.x};
    CHECK_THAT(cross.x, WithinAbs(ez.x, 1e-6f));
    CHECK_THAT(cross.y, WithinAbs(ez.y, 1e-6f));
    CHECK_THAT(cross.z, WithinAbs(ez.z, 1e-6f));
}
