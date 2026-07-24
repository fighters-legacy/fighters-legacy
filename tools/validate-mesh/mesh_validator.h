// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

struct MeshValidationResult {
    bool ok{true};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// One glTF node, flattened for the fl-viewer node-tree panel (#838). glm-free (the header stays
// dependency-light like the rest of the validator lib); the viewer composes transforms itself.
struct MeshNodeInfo {
    std::string name;
    int parent{-1};    // index into MeshNodeTree::nodes, -1 = a root node
    int meshIndex{-1}; // glTF mesh index, -1 = no mesh on this node
    int primitiveCount{0};
    bool damageVariant{false}; // name ends "_b" (the JumpToDamage convention)
    bool engineDrawn{false};   // references mesh 0 (the engine draws meshes[0].primitives[0] only)
    float localMatrix[16]{};   // column-major composed TRS (content frame)
    bool hasAabb{false};
    float aabbMin[3]{};
    float aabbMax[3]{}; // node-local, union of its primitives' POSITION accessor min/max
};

struct MeshNodeTree {
    std::vector<MeshNodeInfo> nodes;
    int meshCount{0};
    int totalPrimitives{0};
};

// Validates a glTF 2.0 file (.glb or .gltf) against engine mesh conventions
// documented in docs/modding/3d-models.md.
//
// validateMesh also discovers and validates LOD sibling files in the same directory
// (e.g. fa18c_lod0.glb when given fa18c.glb).
MeshValidationResult validateMesh(const std::string& filePath);

// Validates glTF 2.0 JSON from an in-memory string — used by unit tests.
MeshValidationResult validateMeshFromJson(std::string_view jsonContent);

// Describe the glTF node hierarchy for the fl-viewer node panel (#838). Returns nullopt when the file
// cannot be parsed as glTF 2.0. Three sources: a file path, an in-memory .glb byte buffer (the
// viewer's pack-bytes path), or JSON text (the test seam).
std::optional<MeshNodeTree> describeMeshNodes(const std::string& filePath);
std::optional<MeshNodeTree> describeMeshNodesFromMemory(const uint8_t* glb, size_t len);
std::optional<MeshNodeTree> describeMeshNodesFromJson(std::string_view jsonContent);

} // namespace fl
