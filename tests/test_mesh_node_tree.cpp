// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "mesh_validator.h"

#include <string>

using namespace fl;

// A minimal glTF 2.0 with three nodes: a parent "fuselage" (mesh 0), a child "canopy" (mesh 1), and a
// damage variant "fuselage_b" (mesh 0). Buffers are empty — describeMeshNodes only walks the node/mesh
// structure + accessor min/max, not vertex data.
static const char* kGltf = R"({
  "asset": {"version": "2.0"},
  "scenes": [{"nodes": [0, 2]}],
  "nodes": [
    {"name": "fuselage", "mesh": 0, "children": [1], "translation": [0, 0, 0]},
    {"name": "canopy", "mesh": 1, "translation": [1, 2, 3]},
    {"name": "fuselage_b", "mesh": 0}
  ],
  "meshes": [
    {"primitives": [{"attributes": {"POSITION": 0}}]},
    {"primitives": [{"attributes": {"POSITION": 1}}]}
  ],
  "accessors": [
    {"componentType": 5126, "count": 3, "type": "VEC3", "min": [-4, -1, -5], "max": [4, 1, 5]},
    {"componentType": 5126, "count": 3, "type": "VEC3", "min": [-1, 0, -1], "max": [1, 2, 1]}
  ]
})";

TEST_CASE("describeMeshNodes walks the hierarchy and flags conventions (#838)") {
    auto treeOpt = describeMeshNodesFromJson(kGltf);
    REQUIRE(treeOpt.has_value());
    const MeshNodeTree& tree = *treeOpt;

    REQUIRE(tree.nodes.size() == 3);
    CHECK(tree.meshCount == 2);
    CHECK(tree.totalPrimitives == 3); // 1 + 1 + 1

    const MeshNodeInfo& fuselage = tree.nodes[0];
    CHECK(fuselage.name == "fuselage");
    CHECK(fuselage.parent == -1); // root
    CHECK(fuselage.meshIndex == 0);
    CHECK(fuselage.engineDrawn); // references mesh 0
    CHECK_FALSE(fuselage.damageVariant);
    CHECK(fuselage.hasAabb);
    CHECK(fuselage.aabbMin[2] == -5.0f);
    CHECK(fuselage.aabbMax[2] == 5.0f);

    const MeshNodeInfo& canopy = tree.nodes[1];
    CHECK(canopy.name == "canopy");
    CHECK(canopy.parent == 0);             // child of fuselage
    CHECK_FALSE(canopy.engineDrawn);       // mesh 1 — the engine draws mesh 0 only
    CHECK(canopy.localMatrix[12] == 1.0f); // translation x baked into the matrix
    CHECK(canopy.localMatrix[13] == 2.0f);
    CHECK(canopy.localMatrix[14] == 3.0f);

    const MeshNodeInfo& dmg = tree.nodes[2];
    CHECK(dmg.name == "fuselage_b");
    CHECK(dmg.damageVariant); // "_b" convention
}

TEST_CASE("describeMeshNodes returns nullopt on unparseable input (#838)") {
    CHECK_FALSE(describeMeshNodesFromJson("not glTF").has_value());
}
