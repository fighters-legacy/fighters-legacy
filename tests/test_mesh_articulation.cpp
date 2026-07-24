// SPDX-License-Identifier: GPL-3.0-or-later
//
// The articulation rig (#840, Epic #837): channel vocabulary, clip parsing and the scrub sampler.
//
// The scrub contract is the thing under test. `t = value x duration` for an unsigned channel and
// `t = (v + 1) / 2 x duration` for a signed one is what makes retraction "scrub gear toward 0"
// rather than a second clip to keep in sync — so the mapping is asserted against hand-computed TRS
// values at the endpoints and the midpoint, for every interpolation mode.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "render/MeshArticulation.h"

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

using namespace fl;

namespace {

std::span<const uint8_t> asBytes(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// A glTF whose animation data lives in a base64 data: URI buffer. Two keyframes of translation on
// node 1 ("gear"): t=0 -> (0,0,0), t=2 -> (0,-4,0). Buffer layout (all float32 LE):
//   [0..8)   times   {0.0, 2.0}
//   [8..32)  values  {0,0,0}, {0,-4,0}
// 32 bytes total.
//
// Base64 of those 32 bytes, produced once and pinned here (the byteLength MUST match the decoded
// size or tinygltf rejects the buffer -- the fixture trap this codebase has hit before).
const char* kGearGltf = R"({
  "asset": {"version": "2.0"},
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"name": "fuselage", "mesh": 0, "children": [1]},
    {"name": "gear"},
    {"name": "gear_b"}
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
    "channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}}]
  }]
})";

// Same buffer, STEP interpolation.
std::string stepVariant() {
    std::string s = kGearGltf;
    const std::string from = "\"interpolation\": \"LINEAR\"";
    return s.replace(s.find(from), from.size(), "\"interpolation\": \"STEP\"");
}

// A signed channel (elevator) over the same two keyframes: t=0 is full nose-down, t=2 full nose-up,
// and the clip MIDPOINT is neutral.
std::string elevatorVariant() {
    std::string s = kGearGltf;
    const std::string from = "\"name\": \"gear\",";
    return s.replace(s.find(from), from.size(), "\"name\": \"elevator\",");
}

float poseY(const std::vector<NodePose>& poses, uint32_t node) {
    for (const NodePose& p : poses)
        if (p.nodeIndex == node)
            return p.localTransform[3][1];
    return 12345.0f; // a value no fixture produces, so a missing pose fails loudly
}

} // namespace

TEST_CASE("ArtChannel names round-trip and the spin aliases collapse (#840)") {
    for (std::size_t i = 0; i < kArtChannelCount; ++i) {
        const auto c = static_cast<ArtChannel>(i);
        CHECK_FALSE(artChannelName(c).empty());
        CHECK(artChannelFromName(artChannelName(c)) == c);
    }
    // One looping playhead, three authoring names.
    CHECK(artChannelFromName("rotor_spin") == ArtChannel::PropRate);
    CHECK(artChannelFromName("wheel_spin") == ArtChannel::PropRate);
    // An unknown name resolves to the sentinel, never to a plausible neighbour.
    CHECK(artChannelFromName("gear_extend") == ArtChannel::kCount);
    CHECK(artChannelFromName("") == ArtChannel::kCount);

    CHECK(artChannelIsSigned(ArtChannel::Elevator));
    CHECK_FALSE(artChannelIsSigned(ArtChannel::Gear));
    CHECK(artChannelIsSpin(ArtChannel::PropRate));
    CHECK(isArtChannelOrdinal(0));
    CHECK_FALSE(isArtChannelOrdinal(static_cast<uint8_t>(ArtChannel::kCount)));
}

TEST_CASE("buildArticulationRig parses a named clip and its duration (#840)") {
    const std::string json = kGearGltf;
    const ArticulationRig rig = buildArticulationRig(asBytes(json));
    REQUIRE(rig.valid);
    REQUIRE(rig.hasChannel(ArtChannel::Gear));
    CHECK_FALSE(rig.hasChannel(ArtChannel::Flaps)); // absent channels stay absent
    const ArtClip& clip = rig.clips[0];
    CHECK(clip.duration == Catch::Approx(2.0));
    REQUIRE(clip.samplers.size() == 1);
    CHECK(clip.samplers[0].nodeIndex == 1u);
    CHECK(clip.samplers[0].values.size() == 2);
}

TEST_CASE("sampleClip scrubs an unsigned channel across its range (#840)") {
    const std::string json = kGearGltf;
    const ArticulationRig rig = buildArticulationRig(asBytes(json));
    REQUIRE(rig.valid);

    // The whole point of the contract: retraction is scrubbing toward 0, not a second clip.
    std::vector<NodePose> poses;
    sampleClip(rig, ArtChannel::Gear, 0.0f, poses);
    CHECK(poseY(poses, 1) == Catch::Approx(0.0));

    poses.clear();
    sampleClip(rig, ArtChannel::Gear, 0.25f, poses);
    CHECK(poseY(poses, 1) == Catch::Approx(-1.0));

    poses.clear();
    sampleClip(rig, ArtChannel::Gear, 0.5f, poses);
    CHECK(poseY(poses, 1) == Catch::Approx(-2.0));

    poses.clear();
    sampleClip(rig, ArtChannel::Gear, 1.0f, poses);
    CHECK(poseY(poses, 1) == Catch::Approx(-4.0));
}

TEST_CASE("sampleClip clamps rather than extrapolating (#840)") {
    const std::string json = kGearGltf;
    const ArticulationRig rig = buildArticulationRig(asBytes(json));
    std::vector<NodePose> poses;
    sampleClip(rig, ArtChannel::Gear, 5.0f, poses);
    CHECK(poseY(poses, 1) == Catch::Approx(-4.0)); // not -20
    poses.clear();
    sampleClip(rig, ArtChannel::Gear, -3.0f, poses);
    CHECK(poseY(poses, 1) == Catch::Approx(0.0));
}

TEST_CASE("sampleClip maps a signed channel through the clip midpoint (#840)") {
    const std::string json = elevatorVariant();
    const ArticulationRig rig = buildArticulationRig(asBytes(json));
    REQUIRE(rig.valid);
    REQUIRE(rig.hasChannel(ArtChannel::Elevator));

    // v = -1 -> t = 0 (full negative), v = 0 -> t = duration/2 (NEUTRAL), v = +1 -> t = duration.
    std::vector<NodePose> poses;
    sampleClip(rig, ArtChannel::Elevator, -1.0f, poses);
    CHECK(poseY(poses, 1) == Catch::Approx(0.0));

    poses.clear();
    sampleClip(rig, ArtChannel::Elevator, 0.0f, poses);
    CHECK(poseY(poses, 1) == Catch::Approx(-2.0));

    poses.clear();
    sampleClip(rig, ArtChannel::Elevator, 1.0f, poses);
    CHECK(poseY(poses, 1) == Catch::Approx(-4.0));
}

TEST_CASE("sampleClip honours STEP interpolation (#840)") {
    const std::string json = stepVariant();
    const ArticulationRig rig = buildArticulationRig(asBytes(json));
    REQUIRE(rig.valid);
    std::vector<NodePose> poses;
    // Halfway between the keyframes STEP holds the earlier value; LINEAR would give -2.
    sampleClip(rig, ArtChannel::Gear, 0.5f, poses);
    CHECK(poseY(poses, 1) == Catch::Approx(0.0));
    poses.clear();
    sampleClip(rig, ArtChannel::Gear, 1.0f, poses);
    CHECK(poseY(poses, 1) == Catch::Approx(-4.0));
}

TEST_CASE("sampleClip evaluates CUBICSPLINE against the glTF Hermite basis (#840)") {
    // Three values per keyframe (in-tangent, value, out-tangent). Tangents chosen so the spline
    // DIFFERS from the linear result at the midpoint (-4 vs -2), or the test would pass on a
    // silently-linear implementation:
    //   h(0.5) = 0.5*v0 + span*0.125*m0 + 0.5*v1 + span*(-0.125)*m1
    //          = 0        + 2*0.125*(-8) + 0.5*(-4) + 0            = -4
    const char* json = R"({
      "asset": {"version": "2.0"},
      "scenes": [{"nodes": [0]}],
      "nodes": [{"name": "fuselage", "mesh": 0, "children": [1]}, {"name": "gear"}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
      "accessors": [
        {"componentType": 5126, "count": 3, "type": "VEC3"},
        {"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0.0], "max": [2.0]},
        {"bufferView": 1, "componentType": 5126, "count": 6, "type": "VEC3"}
      ],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 8},
        {"buffer": 0, "byteOffset": 8, "byteLength": 72}
      ],
      "buffers": [{"byteLength": 80, "uri": "data:application/octet-stream;base64,AAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAwQAAAAAAAAAAAAAAAAAAAAAAAAAAAACAwAAAAAAAAAAAAAAAAAAAAAA="}],
      "animations": [{
        "name": "gear",
        "samplers": [{"input": 1, "output": 2, "interpolation": "CUBICSPLINE"}],
        "channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}}]
      }]
    })";
    const std::string s = json;
    const ArticulationRig rig = buildArticulationRig(asBytes(s));
    REQUIRE(rig.valid);
    REQUIRE(rig.hasChannel(ArtChannel::Gear));

    std::vector<NodePose> poses;
    sampleClip(rig, ArtChannel::Gear, 0.0f, poses);
    CHECK(poseY(poses, 1) == Catch::Approx(0.0));
    poses.clear();
    sampleClip(rig, ArtChannel::Gear, 0.5f, poses);
    CHECK(poseY(poses, 1) == Catch::Approx(-4.0));
    poses.clear();
    sampleClip(rig, ArtChannel::Gear, 1.0f, poses);
    CHECK(poseY(poses, 1) == Catch::Approx(-4.0));
}

TEST_CASE("one sampleClip call poses every node the clip targets (#840)") {
    // A gear clip sequences struts, doors and linkages: several nodes, one channel, one call.
    const char* json = R"({
      "asset": {"version": "2.0"},
      "scenes": [{"nodes": [0]}],
      "nodes": [
        {"name": "fuselage", "mesh": 0, "children": [1, 2]},
        {"name": "strut"},
        {"name": "door"}
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
        "channels": [
          {"sampler": 0, "target": {"node": 1, "path": "translation"}},
          {"sampler": 0, "target": {"node": 2, "path": "translation"}}
        ]
      }]
    })";
    const std::string s = json;
    const ArticulationRig rig = buildArticulationRig(asBytes(s));
    REQUIRE(rig.valid);
    std::vector<NodePose> poses;
    sampleClip(rig, ArtChannel::Gear, 1.0f, poses);
    CHECK(poses.size() == 2);
    CHECK(poseY(poses, 1) == Catch::Approx(-4.0));
    CHECK(poseY(poses, 2) == Catch::Approx(-4.0));
    CHECK(rig.poseCapacity >= 2);
}

TEST_CASE("a damage node inherits its base node's sampled pose (#840)") {
    // A damaged flap still hangs on the same hinge, so "<name>_b" copies <name>'s pose and the
    // author never duplicates the clip.
    const std::string json = kGearGltf;
    const ArticulationRig rig = buildArticulationRig(asBytes(json));
    REQUIRE(rig.valid);
    REQUIRE(rig.damageBaseOf.size() == 3);
    CHECK(rig.damageBaseOf[2] == 1); // "gear_b" mirrors "gear"

    std::vector<NodePose> poses;
    sampleClip(rig, ArtChannel::Gear, 1.0f, poses);
    CHECK(poseY(poses, 1) == Catch::Approx(-4.0));
    CHECK(poseY(poses, 2) == Catch::Approx(-4.0));
}

TEST_CASE("a mesh with zero animations builds an empty rig and costs nothing (#840)") {
    // The static-mesh compatibility baseline: f5e.glb as shipped contains no animations, and must
    // stay valid forever.
    const char* json = R"({
      "asset": {"version": "2.0"},
      "scenes": [{"nodes": [0]}],
      "nodes": [{"name": "f5e", "mesh": 0}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
      "accessors": [{"componentType": 5126, "count": 3, "type": "VEC3"}]
    })";
    const std::string s = json;
    const ArticulationRig rig = buildArticulationRig(asBytes(s));
    CHECK(rig.valid);
    CHECK(rig.empty());
    CHECK(rig.poseCapacity == 0);
    for (std::size_t i = 0; i < kArtChannelCount; ++i)
        CHECK_FALSE(rig.hasChannel(static_cast<ArtChannel>(i)));

    // Sampling an absent channel appends nothing — that is what lets the simulation drive all
    // sixteen channels at every entity regardless of what its mesh models.
    std::vector<NodePose> poses;
    sampleClip(rig, ArtChannel::Gear, 1.0f, poses);
    CHECK(poses.empty());
}

TEST_CASE("a skinned mesh is rejected with a diagnostic (#840)") {
    const char* json = R"({
      "asset": {"version": "2.0"},
      "scenes": [{"nodes": [0]}],
      "nodes": [{"name": "arm", "mesh": 0}, {"name": "joint"}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
      "accessors": [{"componentType": 5126, "count": 3, "type": "VEC3"}],
      "skins": [{"joints": [1]}],
      "animations": [{"name": "gear", "samplers": [], "channels": []}]
    })";
    const std::string s = json;
    const ArticulationRig rig = buildArticulationRig(asBytes(s));
    CHECK_FALSE(rig.valid);
    CHECK(rig.error.find("skin") != std::string::npos);
}

TEST_CASE("an unknown clip name is ignored rather than mis-bound (#840)") {
    std::string s = kGearGltf;
    const std::string from = "\"name\": \"gear\",";
    s = s.replace(s.find(from), from.size(), "\"name\": \"gear_extend\","); // the retired two-clip name
    const ArticulationRig rig = buildArticulationRig(asBytes(s));
    CHECK(rig.valid);
    CHECK(rig.empty()); // silently unbound at runtime; validate-mesh warns by name at authoring time
}

TEST_CASE("advanceSpinPhase loops and stays finite (#840)") {
    // Spin is the one channel that is NOT scrubbed: it is a rate, and the playhead wraps.
    float p = 0.0f;
    p = advanceSpinPhase(p, 0.25f, 2.0f); // half a revolution
    CHECK(p == Catch::Approx(0.5));
    p = advanceSpinPhase(p, 0.25f, 2.0f); // wraps past 1.0
    CHECK(p == Catch::Approx(0.0).margin(1e-5));
    CHECK(advanceSpinPhase(0.5f, 1.0f, 0.0f) == Catch::Approx(0.5)); // rate 0 = stationary
    // Non-finite inputs reset rather than poisoning every subsequent frame.
    CHECK(advanceSpinPhase(std::numeric_limits<float>::quiet_NaN(), 0.1f, 1.0f) == 0.0f);
}
