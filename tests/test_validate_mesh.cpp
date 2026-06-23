// SPDX-License-Identifier: GPL-3.0-or-later
#include "mesh_validator.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

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
