// SPDX-License-Identifier: GPL-3.0-or-later
//
// The articulation sampler's other TRS paths, and what the parser refuses (#1145).
//
// test_mesh_articulation.cpp covers translation clips end to end. Rotation and scale take different
// code paths on the way out — a rotation SLERPS, because a component-wise lerp of a quaternion is
// not a rotation — and a node driven by two samplers at once has to compose into one pose rather
// than have the samplers overwrite each other.
//
// The refusals matter as much. These bytes come from a downloaded content pack, so a mesh that
// declares a skin, animates morph weights, or points a channel at a node that does not exist must
// produce a diagnostic and a static mesh, never a half-rendered aircraft.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "render/MeshArticulation.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

using Catch::Approx;
using namespace fl;

namespace {

std::span<const uint8_t> asBytes(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// One 88-byte buffer serving four clips (all float32 LE):
//   [0..8)   times  {0.0, 1.0}
//   [8..40)  rot    identity, then 90 deg about +Z  (x, y, z, w — glTF order)
//   [40..64) scale  {1,1,1}, {2,2,2}
//   [64..88) trans  {0,0,0}, {3,0,0}
// The byteLength MUST match the decoded size or tinygltf rejects the buffer — the fixture trap this
// codebase has hit before.
constexpr const char* kBuffer64 =
    "AAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAADzBDU/8wQ1PwAAgD8AAIA/AACAPwAAAEAAAABAAAAAQAAA"
    "AAAAAAAAAAAAAAAAQEAAAAAAAAAAAA==";

std::string gltfWith(const std::string& animations, const std::string& extra = {}) {
    return std::string(R"({
  "asset": {"version": "2.0"},
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"name": "root", "children": [1, 2, 3, 4, 5]},
    {"name": "flap"},
    {"name": "brake"},
    {"name": "hinge"},
    {"name": "hinge_b"},
    {"name": "prop"}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0.0], "max": [1.0]},
    {"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC4"},
    {"bufferView": 2, "componentType": 5126, "count": 2, "type": "VEC3"},
    {"bufferView": 3, "componentType": 5126, "count": 2, "type": "VEC3"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0,  "byteLength": 8},
    {"buffer": 0, "byteOffset": 8,  "byteLength": 32},
    {"buffer": 0, "byteOffset": 40, "byteLength": 24},
    {"buffer": 0, "byteOffset": 64, "byteLength": 24}
  ],
  "buffers": [{"byteLength": 88, "uri": "data:application/octet-stream;base64,)") +
           kBuffer64 + R"("}],)" + extra + R"(
  "animations": )" +
           animations + "\n}";
}

const NodePose* poseOf(const std::vector<NodePose>& poses, uint32_t node) {
    for (const NodePose& p : poses)
        if (p.nodeIndex == node)
            return &p;
    return nullptr;
}

// The rotation clip on node 1, bound to `flaps`.
std::string rotationClip(const char* name = "flaps", int node = 1) {
    return std::string(R"([{
    "name": ")") +
           name + R"(",
    "samplers": [{"input": 0, "output": 1, "interpolation": "LINEAR"}],
    "channels": [{"sampler": 0, "target": {"node": )" +
           std::to_string(node) + R"(, "path": "rotation"}}]
  }])";
}

} // namespace

// ---------------------------------------------------------------------------
// The TRS paths
// ---------------------------------------------------------------------------

TEST_CASE("sampleClip slerps a rotation clip rather than lerping its components (#1145)", "[render][art]") {
    // Halfway between identity and 90 degrees about +Z must be 45 degrees. A component-wise lerp of
    // the two quaternions would give an unnormalised value that reads as roughly 45 degrees AND a
    // scale change — the control surface would visibly shrink as it deflected.
    const std::string json = gltfWith(rotationClip());
    const ArticulationRig rig = buildArticulationRig(asBytes(json));
    REQUIRE(rig.valid);
    REQUIRE(rig.hasChannel(ArtChannel::Flaps));

    std::vector<NodePose> poses;
    sampleClip(rig, ArtChannel::Flaps, 0.5f, poses);
    REQUIRE(poses.size() == 1u);
    const glm::mat4& m = poses[0].localTransform;

    // 45 degrees about +Z: the X axis maps to (cos45, sin45, 0), and the basis stays unit length.
    const float c = std::cos(glm::radians(45.0f));
    CHECK(m[0][0] == Approx(c).margin(1e-4));
    CHECK(m[0][1] == Approx(c).margin(1e-4));
    CHECK(glm::length(glm::vec3(m[0])) == Approx(1.0f).margin(1e-4)); // still a rotation
    CHECK(glm::length(glm::vec3(m[1])) == Approx(1.0f).margin(1e-4));

    // The endpoints are exact.
    poses.clear();
    sampleClip(rig, ArtChannel::Flaps, 0.0f, poses);
    CHECK(poses[0].localTransform[0][0] == Approx(1.0f).margin(1e-5)); // identity
    poses.clear();
    sampleClip(rig, ArtChannel::Flaps, 1.0f, poses);
    CHECK(poses[0].localTransform[0][0] == Approx(0.0f).margin(1e-4)); // 90 degrees
    CHECK(poses[0].localTransform[0][1] == Approx(1.0f).margin(1e-4));
}

TEST_CASE("sampleClip drives a scale path (#1145)", "[render][art]") {
    const std::string json = gltfWith(R"([{
    "name": "speedbrake",
    "samplers": [{"input": 0, "output": 2, "interpolation": "LINEAR"}],
    "channels": [{"sampler": 0, "target": {"node": 2, "path": "scale"}}]
  }])");
    const ArticulationRig rig = buildArticulationRig(asBytes(json));
    REQUIRE(rig.valid);
    REQUIRE(rig.hasChannel(ArtChannel::Speedbrake));

    std::vector<NodePose> poses;
    sampleClip(rig, ArtChannel::Speedbrake, 0.0f, poses);
    REQUIRE(poses.size() == 1u);
    CHECK(poses[0].localTransform[0][0] == Approx(1.0f));

    poses.clear();
    sampleClip(rig, ArtChannel::Speedbrake, 1.0f, poses);
    CHECK(poses[0].localTransform[0][0] == Approx(2.0f));
    CHECK(poses[0].localTransform[1][1] == Approx(2.0f));
    CHECK(poses[0].localTransform[2][2] == Approx(2.0f));
}

TEST_CASE("sampleClip composes several samplers on one node into one pose (#1145)", "[render][art]") {
    // A canopy that both slides back and hinges up is two samplers on one node. Emitting two poses
    // would make the last one written win and the other half of the motion vanish.
    const std::string json = gltfWith(R"([{
    "name": "canopy",
    "samplers": [
      {"input": 0, "output": 3, "interpolation": "LINEAR"},
      {"input": 0, "output": 1, "interpolation": "LINEAR"}
    ],
    "channels": [
      {"sampler": 0, "target": {"node": 3, "path": "translation"}},
      {"sampler": 1, "target": {"node": 3, "path": "rotation"}}
    ]
  }])");
    const ArticulationRig rig = buildArticulationRig(asBytes(json));
    REQUIRE(rig.valid);
    REQUIRE(rig.clips.size() == 1u);
    CHECK(rig.clips[0].samplers.size() == 2u);

    std::vector<NodePose> poses;
    sampleClip(rig, ArtChannel::Canopy, 1.0f, poses);

    // One pose for the hinge, plus the mirrored damage node — never two competing poses for node 3.
    const NodePose* hinge = poseOf(poses, 3);
    REQUIRE(hinge != nullptr);
    CHECK(hinge->localTransform[3][0] == Approx(3.0f));              // the translation survived...
    CHECK(hinge->localTransform[0][1] == Approx(1.0f).margin(1e-4)); // ...and so did the rotation

    const NodePose* damaged = poseOf(poses, 4); // "hinge_b"
    REQUIRE(damaged != nullptr);
    CHECK(damaged->localTransform == hinge->localTransform);
    CHECK(poses.size() == 2u);
}

// ---------------------------------------------------------------------------
// Spin
// ---------------------------------------------------------------------------

TEST_CASE("sampleSpin scrubs the looping clip by phase (#1145)", "[render][art]") {
    // Spin is the one channel that loops rather than scrubs, and it is addressed by phase, not by a
    // channel value — so it has its own entry point.
    const std::string json = gltfWith(rotationClip("prop_spin", 5));
    const ArticulationRig rig = buildArticulationRig(asBytes(json));
    REQUIRE(rig.valid);
    REQUIRE(rig.hasChannel(ArtChannel::PropRate));

    std::vector<NodePose> poses;
    sampleSpin(rig, 0.0f, poses);
    REQUIRE(poses.size() == 1u);
    CHECK(poses[0].nodeIndex == 5u);
    CHECK(poses[0].localTransform[0][0] == Approx(1.0f).margin(1e-5));

    poses.clear();
    sampleSpin(rig, 0.5f, poses);
    CHECK(poses[0].localTransform[0][0] == Approx(std::cos(glm::radians(45.0f))).margin(1e-4));

    // A phase past one revolution wraps rather than clamping at the clip end — that is what makes it
    // a loop instead of a scrub that finishes.
    poses.clear();
    sampleSpin(rig, 7.5f, poses);
    CHECK(poses[0].localTransform[0][0] == Approx(std::cos(glm::radians(45.0f))).margin(1e-4));

    poses.clear();
    sampleSpin(rig, -0.5f, poses); // a negative phase wraps too
    CHECK(poses[0].localTransform[0][0] == Approx(std::cos(glm::radians(45.0f))).margin(1e-4));
}

TEST_CASE("sampleSpin on a mesh with no spin clip appends nothing (#1145)", "[render][art]") {
    // Most airframes have no propeller. Sampling an absent channel is a silent no-op, which is what
    // lets the simulation drive all sixteen channels at every entity regardless of its mesh.
    const std::string json = gltfWith(rotationClip());
    const ArticulationRig rig = buildArticulationRig(asBytes(json));
    std::vector<NodePose> poses;
    sampleSpin(rig, 0.25f, poses);
    CHECK(poses.empty());

    sampleClip(rig, ArtChannel::Hook, 1.0f, poses);
    CHECK(poses.empty());
}

TEST_CASE("advanceSpinPhase survives a rate or dt that is not a number (#1145)", "[render][art]") {
    // A NaN reaching the playhead would make the propeller vanish and never come back, because the
    // wrap arithmetic keeps it NaN forever. It resets to zero instead.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(advanceSpinPhase(nan, 0.016f, 5.0f) == 0.0f);
    CHECK(advanceSpinPhase(0.2f, nan, 5.0f) == 0.0f);
    CHECK(advanceSpinPhase(0.2f, 0.016f, nan) == 0.0f);
    CHECK(advanceSpinPhase(0.2f, 0.016f, inf) == 0.0f);

    const float p = advanceSpinPhase(0.9f, 0.5f, 1.0f); // 1.4 -> 0.4
    CHECK(p == Approx(0.4f).margin(1e-5));
    CHECK(p >= 0.0f);
    CHECK(p < 1.0f);

    // A negative rate (a reversing rotor) walks the phase backwards and still lands inside [0, 1).
    const float back = advanceSpinPhase(0.1f, 0.5f, -1.0f);
    CHECK(back >= 0.0f);
    CHECK(back < 1.0f);
}

// ---------------------------------------------------------------------------
// What the parser refuses
// ---------------------------------------------------------------------------

TEST_CASE("buildArticulationRig rejects a morph-target weights channel (#1145)", "[render][art]") {
    // Rigid parts only. Half-rendering a morph rig would look like a modelling mistake to the author
    // rather than an unimplemented feature.
    const std::string json = gltfWith(R"([{
    "name": "flaps",
    "samplers": [{"input": 0, "output": 2, "interpolation": "LINEAR"}],
    "channels": [{"sampler": 0, "target": {"node": 1, "path": "weights"}}]
  }])");
    const ArticulationRig rig = buildArticulationRig(asBytes(json));
    CHECK_FALSE(rig.valid);
    CHECK_FALSE(rig.parseFailed); // the glTF was fine; the RIG refused it
    CHECK(rig.error.find("morph") != std::string::npos);
    CHECK(rig.error.find("flaps") != std::string::npos); // the clip is named so the author can find it
}

TEST_CASE("buildArticulationRig drops a channel pointing at a node that is not there (#1145)", "[render][art]") {
    // An exporter that reordered nodes after the animation was authored. The clip is dropped, not
    // the mesh, and nothing indexes out of bounds.
    const std::string json = gltfWith(R"([{
    "name": "flaps",
    "samplers": [{"input": 0, "output": 1, "interpolation": "LINEAR"}],
    "channels": [
      {"sampler": 0, "target": {"node": 99, "path": "rotation"}},
      {"sampler": 0, "target": {"node": -1, "path": "rotation"}}
    ]
  }])");
    const ArticulationRig rig = buildArticulationRig(asBytes(json));
    CHECK(rig.valid); // still a usable mesh
    CHECK_FALSE(rig.hasChannel(ArtChannel::Flaps));
    CHECK(rig.empty());
}

TEST_CASE("buildArticulationRig drops a channel whose sampler index is out of range (#1145)", "[render][art]") {
    const std::string json = gltfWith(R"([{
    "name": "flaps",
    "samplers": [{"input": 0, "output": 1, "interpolation": "LINEAR"}],
    "channels": [{"sampler": 7, "target": {"node": 1, "path": "rotation"}}]
  }])");
    const ArticulationRig rig = buildArticulationRig(asBytes(json));
    CHECK(rig.valid);
    CHECK_FALSE(rig.hasChannel(ArtChannel::Flaps));
}

TEST_CASE("buildArticulationRig ignores a target path it does not drive (#1145)", "[render][art]") {
    // glTF also allows "weights" (rejected above) and extension paths. Anything that is not one of
    // the three TRS components is skipped rather than guessed at.
    const std::string json = gltfWith(R"([{
    "name": "flaps",
    "samplers": [{"input": 0, "output": 1, "interpolation": "LINEAR"}],
    "channels": [{"sampler": 0, "target": {"node": 1, "path": "pointer"}}]
  }])");
    const ArticulationRig rig = buildArticulationRig(asBytes(json));
    CHECK(rig.valid);
    CHECK_FALSE(rig.hasChannel(ArtChannel::Flaps));
}

TEST_CASE("buildArticulationRig keeps the first of two clips claiming one channel (#1145)", "[render][art]") {
    // A duplicate is a validator error. At runtime it must be deterministic rather than
    // last-writer-wins, so two clients loading the same mesh animate identically.
    const std::string json = gltfWith(R"([
  {
    "name": "flaps",
    "samplers": [{"input": 0, "output": 1, "interpolation": "LINEAR"}],
    "channels": [{"sampler": 0, "target": {"node": 1, "path": "rotation"}}]
  },
  {
    "name": "flaps",
    "samplers": [{"input": 0, "output": 2, "interpolation": "LINEAR"}],
    "channels": [{"sampler": 0, "target": {"node": 2, "path": "scale"}}]
  }])");
    const ArticulationRig rig = buildArticulationRig(asBytes(json));
    REQUIRE(rig.valid);
    REQUIRE(rig.hasChannel(ArtChannel::Flaps));
    REQUIRE(rig.clips.size() == 1u);

    std::vector<NodePose> poses;
    sampleClip(rig, ArtChannel::Flaps, 1.0f, poses);
    REQUIRE(poses.size() == 1u);
    CHECK(poses[0].nodeIndex == 1u); // the first clip's node, not the second's
}

TEST_CASE("buildArticulationRig on bytes that are not glTF says so separately (#1145)", "[render][art]") {
    // parseFailed exists so the rig stays quiet about a failure the mesh loader already reports —
    // otherwise every unreadable model logs the same problem twice.
    const std::string junk = "this is not a model";
    const ArticulationRig bad = buildArticulationRig(asBytes(junk));
    CHECK_FALSE(bad.valid);
    CHECK(bad.parseFailed);

    const ArticulationRig none = buildArticulationRig({});
    CHECK(none.valid); // no bytes at all is not a broken mesh
    CHECK_FALSE(none.parseFailed);
    CHECK(none.empty());
}

// ---------------------------------------------------------------------------
// Pose capacity
// ---------------------------------------------------------------------------

TEST_CASE("poseCapacity bounds what one full sample can produce (#1145)", "[render][art]") {
    // The caller sizes its pose arena from this before sampling. Under-counting would overflow the
    // arena on the frame an aircraft first lowers its gear.
    const std::string json = gltfWith(R"([{
    "name": "canopy",
    "samplers": [
      {"input": 0, "output": 3, "interpolation": "LINEAR"},
      {"input": 0, "output": 1, "interpolation": "LINEAR"}
    ],
    "channels": [
      {"sampler": 0, "target": {"node": 3, "path": "translation"}},
      {"sampler": 1, "target": {"node": 3, "path": "rotation"}}
    ]
  }])");
    const ArticulationRig rig = buildArticulationRig(asBytes(json));
    REQUIRE(rig.valid);

    std::vector<NodePose> poses;
    sampleClip(rig, ArtChannel::Canopy, 1.0f, poses);
    CHECK(poses.size() <= rig.poseCapacity); // two samplers on one node, plus its damage mirror
    CHECK(rig.poseCapacity >= 2u);
}
