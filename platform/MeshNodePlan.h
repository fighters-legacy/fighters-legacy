// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MeshNodePlan — the glTF scene-graph walk that turns a .glb into a per-node draw plan (#839).
//
// Pure and Vulkan-free so it is unit-testable without a GPU: the loader (VkResourceManager::createMesh)
// uploads geometry according to the plan, and the renderer composes the plan's node transforms each
// frame. Everything about "which nodes exist, who is whose parent, what is a damage variant, which
// primitives draw" lives here rather than being tangled into the upload path.
//
// NODE INDICES ARE glTF NODE ARRAY INDICES throughout. That is the contract binding this loader to
// the engine-side articulation sampler (NodePose::nodeIndex, engine/render/MeshArticulation.h): both
// parse the same bytes, so both agree on node order without the HAL having to carry a node table.
//
// Transforms here are in the CONTENT frame (the frame the .glb was authored in). The content→body
// rotation is applied by the renderer (see contentToBodyMatrix in MeshOrient.h), NOT baked into the
// plan, so a clip authored in Blender needs no knowledge of the engine's axis convention.

#include "RenderTypes.h" // NodePose

#include <cstdint>
#include <glm/mat4x4.hpp>
#include <span>
#include <string_view>
#include <vector>

namespace tinygltf {
class Model;
}

namespace fl {

// One glTF node's hierarchy link and authored rest pose.
struct MeshPlanNode {
    int32_t parent{-1};
    glm::mat4 restLocal{1.0f};
    // This node — or an ancestor — is an "_b" damage-variant node: drawn only with kRenderFlagDamaged.
    bool damageVariant{false};
    // A "<name>_b" counterpart exists for this node (or an ancestor): hidden when kRenderFlagDamaged.
    bool shadowedByDamage{false};
    // Reachable from the scene graph AND selected by the variant filter. An absent node keeps its
    // default identity rest transform so a stale NodePose targeting it cannot move live geometry.
    bool present{false};
};

// One primitive to upload, in draw order.
struct MeshPlanPrimitive {
    uint32_t nodeIndex{0};
    int32_t meshIndex{-1};
    int32_t primitiveIndex{-1};
};

struct MeshNodePlan {
    std::vector<MeshPlanNode> nodes;           // indexed by glTF node index; sized to model.nodes
    std::vector<uint32_t> order;               // parents-before-children visit order over present nodes
    std::vector<glm::mat4> globalRest;         // composed rest global per node (content frame)
    std::vector<MeshPlanPrimitive> primitives; // upload/draw order
    bool hasNodeTransforms{false};             // any present node has a non-identity rest local
};

// Walk the default scene (or, absent a scene, every parentless node) depth-first.
//
// `variant` selects the #882 variant node-set: a node whose glTF `extras` carry `fl_variant` (a
// string or an array of strings) is included only when that list contains `variant`; UNTAGGED NODES
// ARE ALWAYS PRESENT (the shared airframe). An excluded node prunes its whole subtree. Empty
// `variant` — the default — therefore selects exactly the untagged set, which is every mesh authored
// before variants existed.
[[nodiscard]] MeshNodePlan buildMeshNodePlan(const tinygltf::Model& model, std::string_view variant = {});

// Compose each present node's global transform, letting `poses` replace a node's rest local TRS
// (#841). `out` is resized to nodes.size(); absent nodes stay identity. One forward pass — `order`
// guarantees parents precede children.
void composeNodeGlobals(const MeshNodePlan& plan, std::span<const NodePose> poses, std::vector<glm::mat4>& out);

} // namespace fl
