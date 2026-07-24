// SPDX-License-Identifier: GPL-3.0-or-later

#include <tiny_gltf.h> // declarations only; the implementation TU is the shared tinygltf-impl target

#include "render/MeshArticulation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <unordered_map>

namespace fl {
namespace {

// Read a FLOAT accessor into a flat vector of `comps`-wide entries. Returns false on any shape the
// glTF spec does not allow for an animation sampler input/output.
bool readFloatAccessor(const tinygltf::Model& model, int accIdx, int comps, std::vector<float>& out) {
    if (accIdx < 0 || accIdx >= static_cast<int>(model.accessors.size()))
        return false;
    const tinygltf::Accessor& acc = model.accessors[static_cast<std::size_t>(accIdx)];
    if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
        return false; // quantized animation data is not in the authoring contract
    const int accComps = tinygltf::GetNumComponentsInType(static_cast<uint32_t>(acc.type));
    if (accComps != comps)
        return false;
    if (acc.bufferView < 0 || acc.bufferView >= static_cast<int>(model.bufferViews.size()))
        return false;
    const tinygltf::BufferView& bv = model.bufferViews[static_cast<std::size_t>(acc.bufferView)];
    if (bv.buffer < 0 || bv.buffer >= static_cast<int>(model.buffers.size()))
        return false;
    const tinygltf::Buffer& buf = model.buffers[static_cast<std::size_t>(bv.buffer)];

    std::size_t stride = static_cast<std::size_t>(acc.ByteStride(bv));
    if (stride == 0)
        stride = sizeof(float) * static_cast<std::size_t>(comps);
    const std::size_t base = bv.byteOffset + acc.byteOffset;

    out.resize(acc.count * static_cast<std::size_t>(comps));
    for (std::size_t i = 0; i < acc.count; ++i) {
        const std::size_t off = base + i * stride;
        if (off + sizeof(float) * static_cast<std::size_t>(comps) > buf.data.size())
            return false;
        std::memcpy(out.data() + i * static_cast<std::size_t>(comps), buf.data.data() + off,
                    sizeof(float) * static_cast<std::size_t>(comps));
    }
    return true;
}

ArtInterp interpFromString(const std::string& s) {
    if (s == "STEP")
        return ArtInterp::Step;
    if (s == "CUBICSPLINE")
        return ArtInterp::CubicSpline;
    return ArtInterp::Linear;
}

// Locate the keyframe pair bracketing `t` and the normalized position between them.
// times is ascending and non-empty.
struct Bracket {
    std::size_t i0{0};
    std::size_t i1{0};
    float u{0.0f};    // [0,1] between i0 and i1
    float span{0.0f}; // t[i1] - t[i0], for the CUBICSPLINE tangent scaling
};

Bracket bracket(const std::vector<float>& times, float t) {
    Bracket b;
    if (times.size() == 1 || t <= times.front())
        return b;
    if (t >= times.back()) {
        b.i0 = b.i1 = times.size() - 1;
        return b;
    }
    const auto it = std::upper_bound(times.begin(), times.end(), t);
    b.i1 = static_cast<std::size_t>(it - times.begin());
    b.i0 = b.i1 - 1;
    b.span = times[b.i1] - times[b.i0];
    b.u = (b.span > 0.0f) ? (t - times[b.i0]) / b.span : 0.0f;
    return b;
}

// glTF's cubic-spline Hermite basis.
glm::vec4 cubicSpline(const glm::vec4& v0, const glm::vec4& outTangent0, const glm::vec4& v1,
                      const glm::vec4& inTangent1, float u, float span) {
    const float u2 = u * u;
    const float u3 = u2 * u;
    return (2.0f * u3 - 3.0f * u2 + 1.0f) * v0 + span * (u3 - 2.0f * u2 + u) * outTangent0 +
           (-2.0f * u3 + 3.0f * u2) * v1 + span * (u3 - u2) * inTangent1;
}

glm::vec4 evaluate(const ArtSampler& s, float t) {
    if (s.times.empty() || s.values.empty())
        return glm::vec4(0.0f);
    const Bracket b = bracket(s.times, t);

    if (s.interp == ArtInterp::CubicSpline) {
        // Three values per keyframe: [inTangent, value, outTangent].
        const std::size_t v0 = b.i0 * 3u + 1u;
        const std::size_t v1 = b.i1 * 3u + 1u;
        if (v1 >= s.values.size())
            return s.values[std::min(v0, s.values.size() - 1u)];
        if (b.i0 == b.i1)
            return s.values[v0];
        return cubicSpline(s.values[v0], s.values[v0 + 1u], s.values[v1], s.values[v1 - 1u], b.u, b.span);
    }

    if (b.i1 >= s.values.size())
        return s.values.back();
    if (s.interp == ArtInterp::Step || b.i0 == b.i1)
        return s.values[b.i0];

    if (s.path == ArtPath::Rotation) {
        // Quaternions slerp; a component-wise lerp of a rotation is not a rotation.
        const glm::vec4& a = s.values[b.i0];
        const glm::vec4& c = s.values[b.i1];
        const glm::quat qa(a.w, a.x, a.y, a.z);
        const glm::quat qc(c.w, c.x, c.y, c.z);
        const glm::quat q = glm::normalize(glm::slerp(qa, qc, b.u));
        return glm::vec4(q.x, q.y, q.z, q.w);
    }
    return glm::mix(s.values[b.i0], s.values[b.i1], b.u);
}

// Accumulates the T/R/S components a clip drives on one node, so several samplers targeting the same
// node compose into ONE pose rather than fighting over it.
struct NodeAccum {
    bool hasT{false}, hasR{false}, hasS{false};
    glm::vec3 t{0.0f};
    glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 s{1.0f};
};

void appendPoses(const ArticulationRig& rig, const ArtClip& clip, float t, std::vector<NodePose>& out) {
    std::unordered_map<uint32_t, NodeAccum> byNode;
    for (const ArtSampler& s : clip.samplers) {
        const glm::vec4 v = evaluate(s, t);
        NodeAccum& a = byNode[s.nodeIndex];
        switch (s.path) {
        case ArtPath::Translation:
            a.hasT = true;
            a.t = glm::vec3(v);
            break;
        case ArtPath::Rotation:
            a.hasR = true;
            a.r = glm::quat(v.w, v.x, v.y, v.z);
            break;
        case ArtPath::Scale:
            a.hasS = true;
            a.s = glm::vec3(v);
            break;
        }
    }

    for (const auto& [node, a] : byNode) {
        glm::mat4 m = glm::translate(glm::mat4(1.0f), a.t) * glm::mat4_cast(a.r);
        if (a.hasS)
            m = glm::scale(m, a.s);
        out.push_back(NodePose{node, m});

        // A damage node inherits its base node's pose (#840): a damaged flap still hangs on the same
        // hinge, so the author never has to duplicate the clip onto "<name>_b".
        for (std::size_t d = 0; d < rig.damageBaseOf.size(); ++d)
            if (rig.damageBaseOf[d] == static_cast<int32_t>(node))
                out.push_back(NodePose{static_cast<uint32_t>(d), m});
    }
}

} // namespace

ArticulationRig buildArticulationRig(std::span<const uint8_t> bytes) {
    ArticulationRig rig;
    rig.clipByChannel.fill(-1);
    if (bytes.empty())
        return rig;

    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;
    bool ok = false;
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "glTF", 4) == 0)
        ok = loader.LoadBinaryFromMemory(&model, &err, &warn, bytes.data(), static_cast<unsigned int>(bytes.size()));
    else
        ok = loader.LoadASCIIFromString(&model, &err, &warn, reinterpret_cast<const char*>(bytes.data()),
                                        static_cast<unsigned int>(bytes.size()), /*base_dir=*/"");
    if (!ok) {
        rig.valid = false;
        rig.error = "glTF parse failed: " + err;
        return rig;
    }

    // A mesh with zero animations builds an empty rig and costs nothing — that is the static-mesh
    // compatibility baseline, and it must stay valid forever.
    if (model.animations.empty())
        return rig;

    // Rigid parts only. A skin means the author reached for joint deformation, which this system does
    // not implement and must not silently half-render.
    if (!model.skins.empty()) {
        rig.valid = false;
        rig.error = "mesh declares a skin; articulation is rigid nodes only (no joint skinning)";
        return rig;
    }

    // Damage-node mirrors: "<base>_b" inherits <base>'s sampled pose.
    rig.damageBaseOf.assign(model.nodes.size(), -1);
    {
        std::unordered_map<std::string, int32_t> byName;
        for (std::size_t i = 0; i < model.nodes.size(); ++i)
            if (!model.nodes[i].name.empty())
                byName.emplace(model.nodes[i].name, static_cast<int32_t>(i));
        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            const std::string& nm = model.nodes[i].name;
            if (nm.size() > 2 && nm.compare(nm.size() - 2, 2, "_b") == 0)
                if (auto it = byName.find(nm.substr(0, nm.size() - 2)); it != byName.end())
                    rig.damageBaseOf[i] = it->second;
        }
    }

    for (const tinygltf::Animation& anim : model.animations) {
        const ArtChannel channel = artChannelFromName(anim.name);
        if (channel == ArtChannel::kCount)
            continue; // unknown clip name: validate-mesh warns by name; the runtime just ignores it
        if (rig.clipByChannel[static_cast<std::size_t>(channel)] >= 0)
            continue; // one clip per channel; a duplicate is a validator error, first wins here

        ArtClip clip;
        for (const tinygltf::AnimationChannel& ch : anim.channels) {
            if (ch.target_path == "weights") {
                rig.valid = false;
                rig.error = "clip \"" + anim.name + "\" animates morph-target weights; rigid nodes only";
                return rig;
            }
            if (ch.target_node < 0 || ch.target_node >= static_cast<int>(model.nodes.size()))
                continue;
            if (ch.sampler < 0 || ch.sampler >= static_cast<int>(anim.samplers.size()))
                continue;
            const tinygltf::AnimationSampler& gs = anim.samplers[static_cast<std::size_t>(ch.sampler)];

            ArtSampler s;
            s.nodeIndex = static_cast<uint32_t>(ch.target_node);
            s.interp = interpFromString(gs.interpolation);
            if (ch.target_path == "translation")
                s.path = ArtPath::Translation;
            else if (ch.target_path == "scale")
                s.path = ArtPath::Scale;
            else if (ch.target_path == "rotation")
                s.path = ArtPath::Rotation;
            else
                continue;

            if (!readFloatAccessor(model, gs.input, 1, s.times))
                continue;
            const int comps = (s.path == ArtPath::Rotation) ? 4 : 3;
            std::vector<float> flat;
            if (!readFloatAccessor(model, gs.output, comps, flat))
                continue;
            s.values.reserve(flat.size() / static_cast<std::size_t>(comps));
            for (std::size_t i = 0; i + static_cast<std::size_t>(comps) <= flat.size();
                 i += static_cast<std::size_t>(comps)) {
                glm::vec4 v(0.0f);
                for (int c = 0; c < comps; ++c)
                    v[c] = flat[i + static_cast<std::size_t>(c)];
                s.values.push_back(v);
            }
            if (s.times.empty() || s.values.empty())
                continue;

            clip.duration = std::max(clip.duration, s.times.back());
            clip.samplers.push_back(std::move(s));
        }

        if (clip.samplers.empty())
            continue;
        rig.clipByChannel[static_cast<std::size_t>(channel)] = static_cast<int32_t>(rig.clips.size());
        rig.clips.push_back(std::move(clip));
    }

    // Pose-arena capacity: distinct target nodes per clip, plus the damage mirrors each can spawn.
    std::size_t mirrors = 0;
    for (int32_t b : rig.damageBaseOf)
        if (b >= 0)
            ++mirrors;
    for (const ArtClip& c : rig.clips) {
        std::vector<uint32_t> distinct;
        for (const ArtSampler& s : c.samplers)
            if (std::find(distinct.begin(), distinct.end(), s.nodeIndex) == distinct.end())
                distinct.push_back(s.nodeIndex);
        rig.poseCapacity += distinct.size() + mirrors;
    }
    return rig;
}

void sampleClip(const ArticulationRig& rig, ArtChannel channel, float value, std::vector<NodePose>& out) {
    const int32_t idx = rig.clipByChannel[static_cast<std::size_t>(channel)];
    if (idx < 0)
        return;
    const ArtClip& clip = rig.clips[static_cast<std::size_t>(idx)];

    // Value → time. Clamp; never extrapolate — a value outside the declared range is a bug upstream,
    // and inventing geometry past the clip's endpoints hides it.
    float v = value;
    if (artChannelIsSigned(channel)) {
        v = std::clamp(v, -1.0f, 1.0f);
        v = (v + 1.0f) * 0.5f;
    } else {
        v = std::clamp(v, 0.0f, 1.0f);
    }
    appendPoses(rig, clip, v * clip.duration, out);
}

float advanceSpinPhase(float phase, float dt, float rate) noexcept {
    if (!std::isfinite(phase) || !std::isfinite(dt) || !std::isfinite(rate))
        return 0.0f;
    float p = phase + rate * dt;
    p -= std::floor(p); // wrap into [0, 1)
    return std::isfinite(p) ? p : 0.0f;
}

void sampleSpin(const ArticulationRig& rig, float phase, std::vector<NodePose>& out) {
    const int32_t idx = rig.clipByChannel[static_cast<std::size_t>(ArtChannel::PropRate)];
    if (idx < 0)
        return;
    const ArtClip& clip = rig.clips[static_cast<std::size_t>(idx)];
    const float p = std::clamp(phase - std::floor(phase), 0.0f, 1.0f);
    appendPoses(rig, clip, p * clip.duration, out);
}

} // namespace fl
