// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MeshArticulation — the articulation rig: clip parsing and the scrub sampler (#840, Epic #837).
//
// ENGINE-SIDE, NOT PLATFORM-SIDE, deliberately: the HAL contract stays `std::span<const NodePose>`,
// and a headless server never builds a rig. The rig is parsed from the SAME .glb bytes the renderer
// uploads, with tinygltf — so `nodeIndex` here is the glTF node array index, the same contract the
// node-aware loader (#839) established, and that is what lets an engine-side sampler address nodes a
// platform-side loader put on the GPU without widening the HAL by a node table.
//
// The runtime SCRUBS: t = value x duration (signed channels centred at the clip midpoint). It never
// "plays" a clip and never invents motion — values arrive already transit-integrated by the
// simulation (#842). The one exception is spin, which loops at a channel-driven rate.

#include "RenderTypes.h" // NodePose
#include "render/ArtChannel.h"

#include <array>
#include <cstdint>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <span>
#include <string>
#include <vector>

namespace fl {

// Which component of a node's TRS a sampler drives.
enum class ArtPath : uint8_t { Translation, Rotation, Scale };

// glTF keyframe interpolation. LINEAR and STEP are required of an authoring tool; CUBICSPLINE is
// accepted (evaluated as the Hermite spline glTF specifies).
enum class ArtInterp : uint8_t { Step, Linear, CubicSpline };

// One animation channel: keyframes driving one path of one node.
struct ArtSampler {
    uint32_t nodeIndex{0};
    ArtPath path{ArtPath::Rotation};
    ArtInterp interp{ArtInterp::Linear};
    std::vector<float> times;
    // One entry per keyframe for STEP/LINEAR; THREE per keyframe for CUBICSPLINE (in-tangent, value,
    // out-tangent), as glTF stores them. Rotations are quaternions in glTF order (x, y, z, w);
    // translations and scales use xyz and ignore w.
    std::vector<glm::vec4> values;
};

// One named clip. Duration is arbitrary except for spin clips, where it is one revolution at rate
// 1.0 — TRANSIT TIMING LIVES IN THE SIMULATION, NEVER IN THE CLIP, so an author retiming a gear clip
// changes nothing about how long the gear takes to travel.
struct ArtClip {
    float duration{0.0f};
    std::vector<ArtSampler> samplers;
};

struct ArticulationRig {
    // Clip index per channel; -1 = the model has no clip for that channel. Sampling an absent channel
    // is a silent no-op, which is what lets the simulation drive all sixteen channels at every entity
    // regardless of what its mesh actually models.
    std::array<int32_t, kArtChannelCount> clipByChannel{};
    std::vector<ArtClip> clips;

    // Per glTF node: the base node an "_b" damage node mirrors (-1 = not a damage node). A damaged
    // flap still hangs on the same hinge, so a damage node inherits its base node's sampled pose
    // rather than needing its own clip.
    std::vector<int32_t> damageBaseOf;

    // Rejected at parse (a skin, or a morph-target `weights` channel): rigid parts only. `error`
    // carries the diagnostic. A rig that simply has no animations is VALID and empty.
    bool valid{true};
    std::string error;

    // Upper bound on the poses one full sample of EVERY present channel can produce — computed at
    // build time. The frame pose arena reserves against this (#841) so the spans handed to FrameScene
    // never dangle behind a reallocation.
    std::size_t poseCapacity{0};

    [[nodiscard]] bool hasChannel(ArtChannel c) const noexcept {
        return clipByChannel[static_cast<std::size_t>(c)] >= 0;
    }
    [[nodiscard]] bool empty() const noexcept {
        return clips.empty();
    }
};

// Build the rig from .glb (or .gltf JSON) bytes. Never throws; a parse failure yields
// `valid == false` with a diagnostic, and the caller falls back to the static mesh.
[[nodiscard]] ArticulationRig buildArticulationRig(std::span<const uint8_t> bytes);

// Scrub `channel` to `value` and APPEND the resulting node poses to `out`.
//
// Value → time: unsigned t = v x duration, signed t = (v + 1) / 2 x duration. Out-of-range values
// clamp; the sampler never extrapolates. An absent channel appends nothing.
//
// A node driven by several of the clip's samplers (translation + rotation, say) yields ONE pose with
// the components composed T·R·S. Damage nodes mirroring a posed base node get a copy of its pose.
void sampleClip(const ArticulationRig& rig, ArtChannel channel, float value, std::vector<NodePose>& out);

// Advance a looping spin playhead. `rate` is in revolutions per second at clip duration 1; the return
// is a phase in [0, 1). Cosmetic and render-side — the simulation never carries a propeller's angle.
[[nodiscard]] float advanceSpinPhase(float phase, float dt, float rate) noexcept;

// Sample a spin clip at an absolute phase in [0, 1). Same append semantics as sampleClip.
void sampleSpin(const ArticulationRig& rig, float phase, std::vector<NodePose>& out);

} // namespace fl
