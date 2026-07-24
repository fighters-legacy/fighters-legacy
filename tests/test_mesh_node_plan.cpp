// SPDX-License-Identifier: GPL-3.0-or-later
//
// MeshNodePlan — the glTF scene-graph walk behind node-aware mesh loading (#839). Pure and GPU-free,
// so the node hierarchy, damage classification and articulation-pose composition are all testable
// without a Vulkan device.

#include <tiny_gltf.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "MeshNodePlan.h"
#include "MeshOrient.h"

#include <string>

using namespace fl;

namespace {

tinygltf::Model parse(const char* json) {
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;
    const bool ok = loader.LoadASCIIFromString(&model, &err, &warn, json, static_cast<unsigned int>(std::strlen(json)),
                                               /*base_dir=*/"");
    REQUIRE(ok);
    return model;
}

// Parent "fuselage" (mesh 0) with a translated child "gear" (mesh 1), plus a damage variant
// "fuselage_b" (mesh 0) as a second root — the f5e.glb shape.
const char* kAircraft = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0, 2]}],
  "nodes": [
    {"name": "fuselage", "mesh": 0, "children": [1]},
    {"name": "gear", "mesh": 1, "translation": [0, -2, 4]},
    {"name": "fuselage_b", "mesh": 0}
  ],
  "meshes": [
    {"primitives": [{"attributes": {"POSITION": 0}}]},
    {"primitives": [{"attributes": {"POSITION": 0}}]}
  ],
  "accessors": [{"componentType": 5126, "count": 3, "type": "VEC3"}]
})";

} // namespace

TEST_CASE("buildMeshNodePlan walks the hierarchy parents-before-children (#839)") {
    const tinygltf::Model model = parse(kAircraft);
    const MeshNodePlan plan = buildMeshNodePlan(model);

    REQUIRE(plan.nodes.size() == 3);
    CHECK(plan.order.size() == 3);
    CHECK(plan.hasNodeTransforms); // "gear" carries a translation

    // The child must never precede its parent in the visit order — that invariant is what lets
    // composeNodeGlobals be a single forward pass.
    std::size_t posFuselage = 0, posGear = 0;
    for (std::size_t i = 0; i < plan.order.size(); ++i) {
        if (plan.order[i] == 0)
            posFuselage = i;
        if (plan.order[i] == 1)
            posGear = i;
    }
    CHECK(posFuselage < posGear);

    CHECK(plan.nodes[0].parent == -1);
    CHECK(plan.nodes[1].parent == 0);
    CHECK(plan.nodes[2].parent == -1);
    CHECK(plan.nodes[0].present);
    CHECK(plan.nodes[1].present);

    // Rest global of the child is the parent chain applied: translation survives to globalRest.
    CHECK(plan.globalRest[1][3][1] == Catch::Approx(-2.0));
    CHECK(plan.globalRest[1][3][2] == Catch::Approx(4.0));
}

TEST_CASE("buildMeshNodePlan emits every mesh-bearing node's primitives (#839)") {
    const tinygltf::Model model = parse(kAircraft);
    const MeshNodePlan plan = buildMeshNodePlan(model);

    // Three mesh-bearing nodes, one primitive each. The pre-#839 loader saw exactly one of them.
    REQUIRE(plan.primitives.size() == 3);
    bool sawGear = false;
    for (const auto& p : plan.primitives)
        if (p.nodeIndex == 1)
            sawGear = true;
    CHECK(sawGear);
}

TEST_CASE("buildMeshNodePlan classifies _b damage nodes and the base they shadow (#839)") {
    const tinygltf::Model model = parse(kAircraft);
    const MeshNodePlan plan = buildMeshNodePlan(model);

    CHECK(plan.nodes[2].damageVariant); // "fuselage_b"
    CHECK_FALSE(plan.nodes[2].shadowedByDamage);
    CHECK(plan.nodes[0].shadowedByDamage); // "fuselage" has a "_b" counterpart
    CHECK_FALSE(plan.nodes[0].damageVariant);
    // The child inherits its parent's classification — a damaged fuselage takes its gear with it.
    CHECK(plan.nodes[1].shadowedByDamage);
}

TEST_CASE("buildMeshNodePlan handles a glTF with no scene declaration (#839)") {
    const char* json = R"({
      "asset": {"version": "2.0"},
      "nodes": [{"name": "a", "mesh": 0}, {"name": "b", "mesh": 0}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
      "accessors": [{"componentType": 5126, "count": 3, "type": "VEC3"}]
    })";
    const MeshNodePlan plan = buildMeshNodePlan(parse(json));
    CHECK(plan.order.size() == 2); // every parentless node is a root
    CHECK(plan.primitives.size() == 2);
}

TEST_CASE("buildMeshNodePlan terminates on a cyclic node reference (#839)") {
    // Malformed content must not hang the loader: `visited` bounds the DFS.
    const char* json = R"({
      "asset": {"version": "2.0"},
      "scenes": [{"nodes": [0]}],
      "nodes": [{"name": "a", "children": [1]}, {"name": "b", "children": [0]}]
    })";
    const MeshNodePlan plan = buildMeshNodePlan(parse(json));
    CHECK(plan.order.size() == 2);
}

// ── articulation pose composition ────────────────────────────────────────────

TEST_CASE("composeNodeGlobals applies rest transforms and honours pose overrides (#841)") {
    const tinygltf::Model model = parse(kAircraft);
    const MeshNodePlan plan = buildMeshNodePlan(model);

    std::vector<glm::mat4> globals;
    composeNodeGlobals(plan, {}, globals);
    REQUIRE(globals.size() == 3);
    CHECK(globals[0] == glm::mat4(1.0f));           // untransformed root
    CHECK(globals[1][3][1] == Catch::Approx(-2.0)); // child rest translation

    // A pose REPLACES the node's rest local, so the child moves to exactly where the clip says.
    NodePose pose{};
    pose.nodeIndex = 1;
    pose.localTransform = glm::mat4(1.0f);
    pose.localTransform[3] = glm::vec4(0.0f, -9.0f, 0.0f, 1.0f);
    const NodePose poses[] = {pose};
    composeNodeGlobals(plan, poses, globals);
    CHECK(globals[1][3][1] == Catch::Approx(-9.0));
    CHECK(globals[1][3][2] == Catch::Approx(0.0)); // the rest translation is gone, not added to
}

TEST_CASE("composeNodeGlobals propagates a parent pose to its children (#841)") {
    const tinygltf::Model model = parse(kAircraft);
    const MeshNodePlan plan = buildMeshNodePlan(model);

    NodePose pose{};
    pose.nodeIndex = 0; // the parent
    pose.localTransform = glm::mat4(1.0f);
    pose.localTransform[3] = glm::vec4(10.0f, 0.0f, 0.0f, 1.0f);
    const NodePose poses[] = {pose};

    std::vector<glm::mat4> globals;
    composeNodeGlobals(plan, poses, globals);
    CHECK(globals[1][3][0] == Catch::Approx(10.0)); // parent translation
    CHECK(globals[1][3][1] == Catch::Approx(-2.0)); // child's own rest translation still applies
}

TEST_CASE("content-to-body conjugation is a proper rotation matching contentForwardToBody (#839)") {
    const glm::mat4 q = contentToBodyMatrix();
    const glm::mat4 qInv = contentToBodyInverse();

    // The matrix must agree with the vector map the vertex path uses, or articulated parts would be
    // placed in a different frame than the geometry they move.
    const glm::vec3 v(1.0f, 2.0f, 3.0f);
    const glm::vec3 mapped = glm::vec3(q * glm::vec4(v, 1.0f));
    const glm::vec3 expect = contentForwardToBody(v);
    CHECK(mapped.x == Catch::Approx(expect.x));
    CHECK(mapped.y == Catch::Approx(expect.y));
    CHECK(mapped.z == Catch::Approx(expect.z));

    CHECK(q * qInv == glm::mat4(1.0f));

    // Conjugating a pure translation gives the translation expressed in the body frame — which is
    // exactly what makes a gear leg that drops along content -Y drop along body -Y in the game.
    glm::mat4 t(1.0f);
    t[3] = glm::vec4(0.0f, -2.0f, 4.0f, 1.0f);
    const glm::mat4 conj = q * t * qInv;
    const glm::vec3 tb = contentForwardToBody(glm::vec3(0.0f, -2.0f, 4.0f));
    CHECK(conj[3][0] == Catch::Approx(tb.x));
    CHECK(conj[3][1] == Catch::Approx(tb.y));
    CHECK(conj[3][2] == Catch::Approx(tb.z));
}
