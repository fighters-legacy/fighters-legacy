// SPDX-License-Identifier: GPL-3.0-or-later

#include <tiny_gltf.h> // declarations only; the implementation TU is the shared tinygltf-impl target

#include "MeshNodePlan.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <unordered_map>

namespace fl {
namespace {

// A node's authored local TRS as a matrix, in the CONTENT frame.
glm::mat4 nodeLocalMatrix(const tinygltf::Node& n) {
    if (n.matrix.size() == 16) {
        glm::mat4 m(1.0f);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                m[c][r] = static_cast<float>(n.matrix[static_cast<std::size_t>(c) * 4u + static_cast<std::size_t>(r)]);
        return m;
    }
    glm::mat4 m(1.0f);
    if (n.translation.size() == 3)
        m = glm::translate(m, glm::vec3(static_cast<float>(n.translation[0]), static_cast<float>(n.translation[1]),
                                        static_cast<float>(n.translation[2])));
    if (n.rotation.size() == 4) {
        // glTF stores a quaternion as [x, y, z, w]; glm::quat's constructor is (w, x, y, z).
        const glm::quat q(static_cast<float>(n.rotation[3]), static_cast<float>(n.rotation[0]),
                          static_cast<float>(n.rotation[1]), static_cast<float>(n.rotation[2]));
        m = m * glm::mat4_cast(q);
    }
    if (n.scale.size() == 3)
        m = glm::scale(m, glm::vec3(static_cast<float>(n.scale[0]), static_cast<float>(n.scale[1]),
                                    static_cast<float>(n.scale[2])));
    return m;
}

} // namespace

MeshNodePlan buildMeshNodePlan(const tinygltf::Model& model) {
    MeshNodePlan plan;
    const std::size_t nodeCount = model.nodes.size();
    plan.nodes.resize(nodeCount);
    plan.globalRest.assign(nodeCount, glm::mat4(1.0f));
    plan.order.reserve(nodeCount);
    if (nodeCount == 0)
        return plan;

    // A node named "<base>_b" is the JumpToDamage variant of "<base>" — the convention validate-mesh
    // already enforces. Classifying it here is what lets kRenderFlagDamaged (set since #886, read by
    // nobody until now) finally select between them. Without it, a node-aware loader would draw f5e
    // and f5e_b superimposed the moment it stopped looking only at meshes[0].
    std::vector<bool> isDamageNode(nodeCount, false);
    std::vector<bool> hasDamageSibling(nodeCount, false);
    {
        std::unordered_map<std::string, std::size_t> byName;
        for (std::size_t i = 0; i < nodeCount; ++i)
            if (!model.nodes[i].name.empty())
                byName.emplace(model.nodes[i].name, i);
        for (std::size_t i = 0; i < nodeCount; ++i) {
            const std::string& nm = model.nodes[i].name;
            if (nm.size() > 2 && nm.compare(nm.size() - 2, 2, "_b") == 0) {
                isDamageNode[i] = true;
                if (auto it = byName.find(nm.substr(0, nm.size() - 2)); it != byName.end())
                    hasDamageSibling[it->second] = true;
            }
        }
    }

    std::vector<uint32_t> roots;
    const int sceneIdx = (model.defaultScene >= 0 && model.defaultScene < static_cast<int>(model.scenes.size()))
                             ? model.defaultScene
                             : (model.scenes.empty() ? -1 : 0);
    if (sceneIdx >= 0) {
        for (int n : model.scenes[static_cast<std::size_t>(sceneIdx)].nodes)
            if (n >= 0 && n < static_cast<int>(nodeCount))
                roots.push_back(static_cast<uint32_t>(n));
    } else {
        // No scene declared (a hand-built .glb): every node with no parent is a root.
        std::vector<bool> isChild(nodeCount, false);
        for (const auto& n : model.nodes)
            for (int c : n.children)
                if (c >= 0 && c < static_cast<int>(nodeCount))
                    isChild[static_cast<std::size_t>(c)] = true;
        for (std::size_t i = 0; i < nodeCount; ++i)
            if (!isChild[i])
                roots.push_back(static_cast<uint32_t>(i));
    }

    // Iterative DFS. Parents are always emitted before their children, so a single forward pass over
    // `order` resolves global transforms at draw time. `visited` also makes a cyclic or duplicated
    // node reference terminate rather than hang the loader on malformed content.
    struct Frame {
        uint32_t node;
        int32_t parent;
        bool damaged;
        bool shadowed;
    };
    std::vector<Frame> stack;
    std::vector<bool> visited(nodeCount, false);
    for (auto it = roots.rbegin(); it != roots.rend(); ++it)
        stack.push_back({*it, -1, false, false});

    while (!stack.empty()) {
        const Frame f = stack.back();
        stack.pop_back();
        if (f.node >= nodeCount || visited[f.node])
            continue;
        const tinygltf::Node& gn = model.nodes[f.node];
        visited[f.node] = true;

        MeshPlanNode& mn = plan.nodes[f.node];
        mn.present = true;
        mn.parent = f.parent;
        mn.restLocal = nodeLocalMatrix(gn);
        mn.damageVariant = f.damaged || isDamageNode[f.node];
        mn.shadowedByDamage = f.shadowed || hasDamageSibling[f.node];
        if (mn.restLocal != glm::mat4(1.0f))
            plan.hasNodeTransforms = true;
        plan.globalRest[f.node] =
            (f.parent >= 0) ? plan.globalRest[static_cast<std::size_t>(f.parent)] * mn.restLocal : mn.restLocal;
        plan.order.push_back(f.node);

        for (auto cit = gn.children.rbegin(); cit != gn.children.rend(); ++cit)
            if (*cit >= 0 && *cit < static_cast<int>(nodeCount))
                stack.push_back(
                    {static_cast<uint32_t>(*cit), static_cast<int32_t>(f.node), mn.damageVariant, mn.shadowedByDamage});
    }

    for (uint32_t n : plan.order) {
        const tinygltf::Node& gn = model.nodes[n];
        if (gn.mesh < 0 || gn.mesh >= static_cast<int>(model.meshes.size()))
            continue;
        const auto& prims = model.meshes[static_cast<std::size_t>(gn.mesh)].primitives;
        for (std::size_t p = 0; p < prims.size(); ++p)
            plan.primitives.push_back({n, gn.mesh, static_cast<int32_t>(p)});
    }
    return plan;
}

void composeNodeGlobals(const MeshNodePlan& plan, std::span<const NodePose> poses, std::vector<glm::mat4>& out) {
    out.assign(plan.nodes.size(), glm::mat4(1.0f));
    for (uint32_t n : plan.order) {
        const MeshPlanNode& node = plan.nodes[n];
        glm::mat4 local = node.restLocal;
        for (const NodePose& pose : poses) {
            if (pose.nodeIndex == n) {
                local = pose.localTransform;
                break;
            }
        }
        out[n] = (node.parent >= 0) ? out[static_cast<std::size_t>(node.parent)] * local : local;
    }
}

} // namespace fl
