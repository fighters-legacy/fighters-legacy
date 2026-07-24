// SPDX-License-Identifier: GPL-3.0-or-later
#include "mesh_validator.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

using namespace fl;

// Minimal valid glTF 2.0 JSON with one mesh node
static const char* kMinimalGltf = R"json({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"name": "fa18c", "mesh": 0}],
  "meshes": [{"name": "fa18c", "primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [{
    "bufferView": 0, "componentType": 5126, "count": 3,
    "type": "VEC3", "max": [1,1,1], "min": [0,0,0]
  }],
  "bufferViews": [{"buffer": 0, "byteLength": 36}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}]
})json";

TEST_CASE("minimal valid glTF 2.0 passes", "[validate-mesh]") {
    auto r = validateMeshFromJson(kMinimalGltf);
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("glTF with asset.version 1.0 fails", "[validate-mesh]") {
    std::string s(kMinimalGltf);
    auto pos = s.find("\"2.0\"");
    REQUIRE(pos != std::string::npos);
    s.replace(pos, 5, "\"1.0\"");
    auto r = validateMeshFromJson(s);
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("2.0") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("glTF with no meshes fails", "[validate-mesh]") {
    // Minimal document with empty meshes array and no nodes referencing a mesh
    auto r = validateMeshFromJson(R"({
        "asset": {"version": "2.0"},
        "meshes": []
    })");
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("mesh") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("node with uppercase name fails", "[validate-mesh]") {
    std::string s(kMinimalGltf);
    // Replace node name with uppercase
    auto pos = s.find("\"fa18c\"");
    REQUIRE(pos != std::string::npos);
    s.replace(pos, 7, "\"FA18C\"");
    // Also fix mesh name to avoid false positive on mesh name check
    auto r = validateMeshFromJson(s);
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("FA18C") != std::string::npos || e.find("lowercase") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("node with space in name fails", "[validate-mesh]") {
    std::string s(kMinimalGltf);
    auto pos = s.find("\"fa18c\"");
    REQUIRE(pos != std::string::npos);
    s.replace(pos, 7, "\"fa 18c\"");
    auto r = validateMeshFromJson(s);
    CHECK_FALSE(r.ok);
}

TEST_CASE("damage-state _b node without base node fails", "[validate-mesh]") {
    // Only has fuselage_b, no fuselage
    auto r = validateMeshFromJson(R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "fuselage_b", "mesh": 0}],
        "meshes": [{"name": "fuselage_b", "primitives": [{"attributes": {"POSITION": 0}}]}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "max": [1,1,1], "min": [0,0,0]}],
        "bufferViews": [{"buffer": 0, "byteLength": 36}],
        "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}]
    })");
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("fuselage_b") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("damage-state _b node with base node passes", "[validate-mesh]") {
    auto r = validateMeshFromJson(R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0, 1]}],
        "nodes": [
            {"name": "fuselage",   "mesh": 0},
            {"name": "fuselage_b", "mesh": 0}
        ],
        "meshes": [{"name": "fuselage", "primitives": [{"attributes": {"POSITION": 0}}]}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "max": [1,1,1], "min": [0,0,0]}],
        "bufferViews": [{"buffer": 0, "byteLength": 36}],
        "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}]
    })");
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("material with unknown extension fails", "[validate-mesh]") {
    auto r = validateMeshFromJson(R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "fa18c", "mesh": 0}],
        "meshes": [{"name": "fa18c", "primitives": [{"attributes": {"POSITION": 0}, "material": 0}]}],
        "materials": [{"name": "fa18c_mat", "extensions": {"MY_custom_extension": {}}}],
        "extensionsUsed": ["MY_custom_extension"],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "max": [1,1,1], "min": [0,0,0]}],
        "bufferViews": [{"buffer": 0, "byteLength": 36}],
        "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}]
    })");
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("MY_custom_extension") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("material with known extension produces warning not error", "[validate-mesh]") {
    auto r = validateMeshFromJson(R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "fa18c", "mesh": 0}],
        "meshes": [{"name": "fa18c", "primitives": [{"attributes": {"POSITION": 0}, "material": 0}]}],
        "materials": [{"name": "fa18c_mat", "extensions": {"KHR_texture_transform": {}}}],
        "extensionsUsed": ["KHR_texture_transform"],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "max": [1,1,1], "min": [0,0,0]}],
        "bufferViews": [{"buffer": 0, "byteLength": 36}],
        "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}]
    })");
    CHECK(r.ok);
    CHECK(r.errors.empty());
    CHECK(!r.warnings.empty());
}

// #833: a spec-conformant KTX2/Basis texture declares KHR_texture_basisu in extensionsUsed. The
// validator must accept it, otherwise authoring the texture reference correctly makes CI fail.
TEST_CASE("KHR_texture_basisu is an accepted extension", "[validate-mesh]") {
    auto r = validateMeshFromJson(R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "fa18c", "mesh": 0}],
        "meshes": [{"name": "fa18c", "primitives": [{"attributes": {"POSITION": 0}}]}],
        "extensionsUsed": ["KHR_texture_basisu"],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "max": [1,1,1], "min": [0,0,0]}],
        "bufferViews": [{"buffer": 0, "byteLength": 36}],
        "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}]
    })");
    CHECK(r.ok);
    for (const auto& e : r.errors)
        CHECK(e.find("KHR_texture_basisu") == std::string::npos);
}

// #833: a texture image URI whose extension is not .ktx2/.png will never load in the engine — warn.
TEST_CASE("texture image URI with an unloadable extension warns", "[validate-mesh]") {
    auto r = validateMeshFromJson(R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "fa18c", "mesh": 0}],
        "meshes": [{"name": "fa18c", "primitives": [{"attributes": {"POSITION": 0}}]}],
        "images": [{"uri": "../../textures/fa18c_diffuse.dds"}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "max": [1,1,1], "min": [0,0,0]}],
        "bufferViews": [{"buffer": 0, "byteLength": 36}],
        "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}]
    })");
    bool found = false;
    for (const auto& w : r.warnings)
        if (w.find(".dds") != std::string::npos || w.find("not a .ktx2 or .png") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

// #833: the authored convention ("../../textures/<name>.ktx2") raises no URI-extension warning.
TEST_CASE("texture image URI ending in .ktx2 raises no extension warning", "[validate-mesh]") {
    auto r = validateMeshFromJson(R"({
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "fa18c", "mesh": 0}],
        "meshes": [{"name": "fa18c", "primitives": [{"attributes": {"POSITION": 0}}]}],
        "images": [{"uri": "../../textures/fa18c_diffuse.ktx2"}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "max": [1,1,1], "min": [0,0,0]}],
        "bufferViews": [{"buffer": 0, "byteLength": 36}],
        "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}]
    })");
    for (const auto& w : r.warnings)
        CHECK(w.find("not a .ktx2 or .png") == std::string::npos);
}

// One triangle p0=(0,0,0) p1=(1,0,0) p2=(0,0,1): winding cross-product = -Y. With normals = -Y
// (consistent / CCW-from-outside) the winding check passes; with normals = +Y the mesh is
// inside-out and must be flagged. Buffer = 3 POSITION vec3 (36 B) + 3 NORMAL vec3 (36 B) = 72 B.
TEST_CASE("mesh with consistent winding passes winding check", "[validate-mesh]") {
    auto r = validateMeshFromJson(R"({
        "asset": {"version": "2.0"},
        "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "hull", "mesh": 0}],
        "meshes": [{"name": "hull", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "mode": 4}]}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,0,1]},
            {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"}
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 36}
        ],
        "buffers": [{"byteLength": 72, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAgL8AAAAAAAAAAAAAgL8AAAAAAAAAAAAAgL8AAAAA"}]
    })");
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("mesh wound inside-out fails winding check", "[validate-mesh]") {
    auto r = validateMeshFromJson(R"({
        "asset": {"version": "2.0"},
        "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "hull", "mesh": 0}],
        "meshes": [{"name": "hull", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "mode": 4}]}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,0,1]},
            {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"}
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 36}
        ],
        "buffers": [{"byteLength": 72, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAgD8AAAAAAAAAAAAAgD8AAAAAAAAAAAAAgD8AAAAA"}]
    })");
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("inside-out") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

// ---------------------------------------------------------------------------
// Articulation clip checks (#844)
//
// Before these, a .glb with a misspelled clip name, a skinned mesh, or a rest pose that disagreed
// with its own t=0 keyframe passed validation clean and then simply did not move in the game, with
// no diagnostic anywhere.
// ---------------------------------------------------------------------------

namespace {

// A rigged aircraft: node 1 ("gear") translates (0,0,0) -> (0,-4,0) over 2 s. `clipName`,
// `interp` and the node's authored rest translation are the knobs the cases below turn.
std::string riggedGltf(const char* clipName, const char* interp = "LINEAR", const char* restTranslation = "[0, 0, 0]",
                       const char* extraNodes = "", const char* extraTop = "") {
    std::string s = R"({
  "asset": {"version": "2.0"},
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"name": "fuselage", "mesh": 0, "children": [1]},
    {"name": "gear", "translation": REST}EXTRANODES
  ],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "accessors": [
    {"componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0.0], "max": [2.0]},
    {"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC3"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 8},
    {"buffer": 0, "byteOffset": 8, "byteLength": 24}
  ],
  "buffers": [{"byteLength": 32, "uri": "data:application/octet-stream;base64,AAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAACAwAAAAAA="}],
  "animations": [{
    "name": "CLIP",
    "samplers": [{"input": 1, "output": 2, "interpolation": "INTERP"}],
    "channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}}]
  }]EXTRATOP
})";
    auto sub = [&s](const std::string& from, const std::string& to) {
        const auto at = s.find(from);
        if (at != std::string::npos)
            s.replace(at, from.size(), to);
    };
    sub("CLIP", clipName);
    sub("INTERP", interp);
    sub("REST", restTranslation);
    sub("EXTRANODES", extraNodes);
    sub("EXTRATOP", extraTop);
    return s;
}

bool hasFinding(const std::vector<std::string>& v, const char* needle) {
    for (const auto& s : v)
        if (s.find(needle) != std::string::npos)
            return true;
    return false;
}

} // namespace

TEST_CASE("validate-mesh: a correctly authored clip passes clean (#844)", "[validate_mesh][articulation]") {
    const auto r = validateMeshFromJson(riggedGltf("gear"));
    CHECK(r.ok);
    CHECK_FALSE(hasFinding(r.warnings, "articulation channel"));
}

TEST_CASE("validate-mesh: an unknown clip name warns and lists the valid channels (#844)",
          "[validate_mesh][articulation]") {
    // The retired two-clip name. It would bind to nothing at runtime and say nothing about it.
    const auto r = validateMeshFromJson(riggedGltf("gear_retract"));
    CHECK(r.ok); // a warning, not an error — an author may ship an unrelated clip
    REQUIRE(hasFinding(r.warnings, "gear_retract"));
    CHECK(hasFinding(r.warnings, "speedbrake")); // the valid-name list is printed
}

TEST_CASE("validate-mesh: an unsupported interpolation is an error (#844)", "[validate_mesh][articulation]") {
    const auto r = validateMeshFromJson(riggedGltf("gear", "BOGUS"));
    CHECK_FALSE(r.ok);
    CHECK(hasFinding(r.errors, "interpolation"));
    // The three the engine evaluates are all accepted.
    CHECK(validateMeshFromJson(riggedGltf("gear", "STEP")).ok);
    CHECK(validateMeshFromJson(riggedGltf("gear", "LINEAR")).ok);
}

TEST_CASE("validate-mesh: a rest pose disagreeing with the neutral keyframe is an error (#844)",
          "[validate_mesh][articulation]") {
    // The single most likely authoring mistake: the aircraft renders wrong before anything is
    // commanded, and nothing at runtime would ever say so.
    const auto r = validateMeshFromJson(riggedGltf("gear", "LINEAR", "[0, -2, 0]"));
    CHECK_FALSE(r.ok);
    CHECK(hasFinding(r.errors, "rest translation"));
}

TEST_CASE("validate-mesh: a signed channel's neutral is the clip MIDPOINT (#844)", "[validate_mesh][articulation]") {
    // elevator is signed: t=0 is full nose-down, the middle keyframe is neutral. A rest pose equal to
    // the FIRST keyframe would be correct for `gear` and wrong here — which is exactly the trap.
    const char* json = R"({
      "asset": {"version": "2.0"},
      "scenes": [{"nodes": [0]}],
      "nodes": [{"name": "fuselage", "mesh": 0, "children": [1]}, {"name": "stab", "translation": [0, 0, 0]}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
      "accessors": [
        {"componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "SCALAR", "min": [0.0], "max": [2.0]},
        {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"}
      ],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 12},
        {"buffer": 0, "byteOffset": 12, "byteLength": 36}
      ],
      "buffers": [{"byteLength": 48, "uri": "data:application/octet-stream;base64,AAAAAAAAgD8AAABAAAAAAAAAgMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgEAAAAAA"}],
      "animations": [{
        "name": "elevator",
        "samplers": [{"input": 1, "output": 2, "interpolation": "LINEAR"}],
        "channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}}]
      }]
    })";
    // Rest (0,0,0) matches the MIDDLE keyframe (neutral) — correct.
    CHECK(validateMeshFromJson(json).ok);

    // Rest matching the FIRST keyframe (full negative deflection) is the mistake.
    std::string bad = json;
    const auto at = bad.find("\"name\": \"stab\", \"translation\": [0, 0, 0]");
    REQUIRE(at != std::string::npos);
    bad.replace(at, std::strlen("\"name\": \"stab\", \"translation\": [0, 0, 0]"),
                "\"name\": \"stab\", \"translation\": [0, -4, 0]");
    const auto r = validateMeshFromJson(bad);
    CHECK_FALSE(r.ok);
    CHECK(hasFinding(r.errors, "MIDPOINT"));
}

TEST_CASE("validate-mesh: a skin is an error (#844)", "[validate_mesh][articulation]") {
    const auto r =
        validateMeshFromJson(riggedGltf("gear", "LINEAR", "[0, 0, 0]", "", ",\n  \"skins\": [{\"joints\": [1]}]"));
    CHECK_FALSE(r.ok);
    CHECK(hasFinding(r.errors, "skin"));
}

TEST_CASE("validate-mesh: a clip targeting a _b damage node directly warns (#844)", "[validate_mesh][articulation]") {
    // "_b" nodes inherit their base node's sampled pose, so animating one is redundant work the
    // engine overwrites.
    const char* json = R"({
      "asset": {"version": "2.0"},
      "scenes": [{"nodes": [0]}],
      "nodes": [
        {"name": "fuselage", "mesh": 0, "children": [1, 2]},
        {"name": "gear", "translation": [0, 0, 0]},
        {"name": "gear_b", "translation": [0, 0, 0]}
      ],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
      "accessors": [
        {"componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0.0], "max": [2.0]},
        {"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC3"}
      ],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 8},
        {"buffer": 0, "byteOffset": 8, "byteLength": 24}
      ],
      "buffers": [{"byteLength": 32, "uri": "data:application/octet-stream;base64,AAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAACAwAAAAAA="}],
      "animations": [{
        "name": "gear",
        "samplers": [{"input": 1, "output": 2, "interpolation": "LINEAR"}],
        "channels": [{"sampler": 0, "target": {"node": 2, "path": "translation"}}]
      }]
    })";
    const auto r = validateMeshFromJson(json);
    CHECK(hasFinding(r.warnings, "gear_b"));
}

TEST_CASE("validate-mesh: two scrubbed clips on one node is an error (#844)", "[validate_mesh][articulation]") {
    const char* json = R"({
      "asset": {"version": "2.0"},
      "scenes": [{"nodes": [0]}],
      "nodes": [{"name": "fuselage", "mesh": 0, "children": [1]}, {"name": "gear", "translation": [0, 0, 0]}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
      "accessors": [
        {"componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0.0], "max": [2.0]},
        {"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC3"}
      ],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 8},
        {"buffer": 0, "byteOffset": 8, "byteLength": 24}
      ],
      "buffers": [{"byteLength": 32, "uri": "data:application/octet-stream;base64,AAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAACAwAAAAAA="}],
      "animations": [
        {"name": "gear", "samplers": [{"input": 1, "output": 2, "interpolation": "LINEAR"}],
         "channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}}]},
        {"name": "flaps", "samplers": [{"input": 1, "output": 2, "interpolation": "LINEAR"}],
         "channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}}]}
      ]
    })";
    const auto r = validateMeshFromJson(json);
    CHECK_FALSE(r.ok);
    CHECK(hasFinding(r.errors, "two scrubbed clips"));
}

TEST_CASE("validate-mesh: a marker empty with extras is legal (#844)", "[validate_mesh][articulation]") {
    // A node with no mesh (a camera anchor, a hardpoint or hinge marker) and arbitrary glTF extras
    // are forward-compatible metadata, not errors.
    const char* json = R"({
      "asset": {"version": "2.0"},
      "scenes": [{"nodes": [0]}],
      "nodes": [
        {"name": "fuselage", "mesh": 0, "children": [1]},
        {"name": "camera_anchor", "translation": [1, 2, 3], "extras": {"fl_note": "pilot eye point"}}
      ],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
      "accessors": [{"componentType": 5126, "count": 3, "type": "VEC3"}]
    })";
    const auto r = validateMeshFromJson(json);
    CHECK(r.ok);
}

TEST_CASE("validate-mesh: a mesh with zero animations stays valid (#844)", "[validate_mesh][articulation]") {
    // The static baseline: f5e.glb as shipped has no animations and must pass forever.
    const char* json = R"({
      "asset": {"version": "2.0"},
      "scenes": [{"nodes": [0]}],
      "nodes": [{"name": "f5e", "mesh": 0}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
      "accessors": [{"componentType": 5126, "count": 3, "type": "VEC3"}]
    })";
    CHECK(validateMeshFromJson(json).ok);
}
