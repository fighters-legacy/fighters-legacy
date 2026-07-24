// SPDX-License-Identifier: GPL-3.0-or-later

// tinygltf DECLARATIONS only — the single implementation lives in the shared `tinygltf-impl` target
// (third_party/tinygltf_impl.cpp), which validate-mesh-lib links. This lets fl-viewer link both this
// lib (the node tree + diagnostics) AND platform-vulkan (the renderer) without the two tinygltf
// implementations colliding at link time (#836).
#include <tiny_gltf.h>

#include "mesh_validator.h"

#include "render/ArtChannel.h" // #844: the channel vocabulary the clips must name (header-only, stdlib)

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace fl {

// ── constants ─────────────────────────────────────────────────────────────────

static constexpr std::uintmax_t kWarnFileSizeBytes = 50ULL * 1024ULL * 1024ULL;

// ── helpers ───────────────────────────────────────────────────────────────────

static bool isLowercaseUnderscored(const std::string& name) {
    for (char c : name) {
        if (c == ' ' || c == '\t')
            return false;
        if (c >= 'A' && c <= 'Z')
            return false;
    }
    return true;
}

static bool hasEmbeddedImages(const tinygltf::Model& model) {
    for (const auto& img : model.images) {
        // An image with a non-empty image vector has embedded data
        if (!img.image.empty())
            return true;
        // A bufferView index >= 0 means embedded in the GLB buffer
        if (img.bufferView >= 0)
            return true;
    }
    return false;
}

// ── winding-consistency check ───────────────────────────────────────────────────
//
// glTF / engine convention: triangle faces are wound CCW-from-outside, so each face's winding
// cross-product agrees with its (outward) vertex normals. The opaque pipeline is single-sided
// (cull BACK, frontFace=CCW), so a mesh wound inside-out renders with its front faces culled
// (you see through to the interior / far faces). This is the single most common authoring
// mistake (Blender "flipped normals"), so flag it here.

using Vec3 = std::array<double, 3>;

// Read a VEC3/FLOAT accessor into out. Returns false if the accessor is missing/not VEC3 float.
static bool readVec3Accessor(const tinygltf::Model& model, int idx, std::vector<Vec3>& out) {
    if (idx < 0 || idx >= static_cast<int>(model.accessors.size()))
        return false;
    const tinygltf::Accessor& acc = model.accessors[idx];
    if (acc.type != TINYGLTF_TYPE_VEC3 || acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
        return false;
    if (acc.bufferView < 0 || acc.bufferView >= static_cast<int>(model.bufferViews.size()))
        return false;
    const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
    if (bv.buffer < 0 || bv.buffer >= static_cast<int>(model.buffers.size()))
        return false;
    const tinygltf::Buffer& buf = model.buffers[bv.buffer];
    std::size_t stride = static_cast<std::size_t>(acc.ByteStride(bv));
    if (stride == 0)
        stride = sizeof(float) * 3;
    const std::size_t base = bv.byteOffset + acc.byteOffset;
    out.resize(acc.count);
    for (std::size_t i = 0; i < acc.count; ++i) {
        const std::size_t off = base + i * stride;
        if (off + sizeof(float) * 3 > buf.data.size())
            return false;
        float v[3];
        std::memcpy(v, buf.data.data() + off, sizeof(v));
        out[i] = {static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2])};
    }
    return true;
}

// Read a SCALAR index accessor (UNSIGNED_BYTE/SHORT/INT) into out.
static bool readIndexAccessor(const tinygltf::Model& model, int idx, std::vector<uint32_t>& out) {
    if (idx < 0 || idx >= static_cast<int>(model.accessors.size()))
        return false;
    const tinygltf::Accessor& acc = model.accessors[idx];
    if (acc.type != TINYGLTF_TYPE_SCALAR)
        return false;
    if (acc.bufferView < 0 || acc.bufferView >= static_cast<int>(model.bufferViews.size()))
        return false;
    const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
    if (bv.buffer < 0 || bv.buffer >= static_cast<int>(model.buffers.size()))
        return false;
    const tinygltf::Buffer& buf = model.buffers[bv.buffer];
    const int compSize = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(acc.componentType));
    if (compSize <= 0)
        return false;
    std::size_t stride = static_cast<std::size_t>(acc.ByteStride(bv));
    if (stride == 0)
        stride = static_cast<std::size_t>(compSize);
    const std::size_t base = bv.byteOffset + acc.byteOffset;
    out.resize(acc.count);
    for (std::size_t i = 0; i < acc.count; ++i) {
        const std::size_t off = base + i * stride;
        if (off + static_cast<std::size_t>(compSize) > buf.data.size())
            return false;
        uint32_t v = 0;
        if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
            v = buf.data[off];
        } else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
            uint16_t s = 0;
            std::memcpy(&s, buf.data.data() + off, sizeof(s));
            v = s;
        } else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
            std::memcpy(&v, buf.data.data() + off, sizeof(v));
        } else {
            return false;
        }
        out[i] = v;
    }
    return true;
}

static void checkWindingConsistency(const tinygltf::Model& model, const std::string& label, MeshValidationResult& r) {
    for (const auto& mesh : model.meshes) {
        for (const auto& prim : mesh.primitives) {
            // Default mode is TRIANGLES (4); tinygltf uses -1 when unset.
            if (prim.mode != TINYGLTF_MODE_TRIANGLES && prim.mode != -1)
                continue;
            const auto posIt = prim.attributes.find("POSITION");
            const auto nrmIt = prim.attributes.find("NORMAL");
            if (posIt == prim.attributes.end() || nrmIt == prim.attributes.end())
                continue; // need both positions and normals to compare winding

            std::vector<Vec3> pos, nrm;
            if (!readVec3Accessor(model, posIt->second, pos) || !readVec3Accessor(model, nrmIt->second, nrm))
                continue;
            if (pos.size() != nrm.size() || pos.empty())
                continue;

            std::vector<uint32_t> indices;
            const bool indexed = prim.indices >= 0 && readIndexAccessor(model, prim.indices, indices);
            const std::size_t triCount = indexed ? indices.size() / 3 : pos.size() / 3;

            int consistent = 0, inverted = 0;
            for (std::size_t t = 0; t < triCount; ++t) {
                const uint32_t i0 = indexed ? indices[t * 3 + 0] : static_cast<uint32_t>(t * 3 + 0);
                const uint32_t i1 = indexed ? indices[t * 3 + 1] : static_cast<uint32_t>(t * 3 + 1);
                const uint32_t i2 = indexed ? indices[t * 3 + 2] : static_cast<uint32_t>(t * 3 + 2);
                if (i0 >= pos.size() || i1 >= pos.size() || i2 >= pos.size())
                    continue;
                const Vec3& p0 = pos[i0];
                const Vec3& p1 = pos[i1];
                const Vec3& p2 = pos[i2];
                const double e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
                const double e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
                const double cx[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                      e1[0] * e2[1] - e1[1] * e2[0]};
                const double cxLen = std::sqrt(cx[0] * cx[0] + cx[1] * cx[1] + cx[2] * cx[2]);
                if (cxLen < 1e-12)
                    continue; // degenerate triangle
                // Average vertex normal of the face.
                const double na[3] = {(nrm[i0][0] + nrm[i1][0] + nrm[i2][0]) / 3.0,
                                      (nrm[i0][1] + nrm[i1][1] + nrm[i2][1]) / 3.0,
                                      (nrm[i0][2] + nrm[i1][2] + nrm[i2][2]) / 3.0};
                const double naLen = std::sqrt(na[0] * na[0] + na[1] * na[1] + na[2] * na[2]);
                if (naLen < 1e-6)
                    continue; // zero/degenerate normal
                const double d = cx[0] * na[0] + cx[1] * na[1] + cx[2] * na[2];
                if (d > 0.0)
                    ++consistent;
                else if (d < 0.0)
                    ++inverted;
            }

            const int total = consistent + inverted;
            if (total == 0)
                continue;
            const std::string meshName = mesh.name.empty() ? "<unnamed>" : mesh.name;
            const std::string counts = std::to_string(inverted) + "/" + std::to_string(total);
            if (inverted * 2 > total) {
                // Majority of faces wound opposite their normals — the mesh is inside-out.
                r.errors.push_back(label + ": mesh \"" + meshName + "\" is wound inside-out (" + counts +
                                   " triangles have winding opposite their normals). Faces must be "
                                   "CCW-from-outside with outward normals (Blender: Mesh > Normals > "
                                   "Recalculate Outside). See docs/modding/3d-models.md.");
                r.ok = false;
            } else if (inverted * 20 > total) {
                // >5% inconsistent — likely some flipped faces.
                r.warnings.push_back(label + ": mesh \"" + meshName + "\" has " + counts +
                                     " triangles wound opposite their normals (possible flipped faces).");
            }
        }
    }
}

// ── articulation clip checks (#844) ───────────────────────────────────────────
//
// The engine's contract (engine/render/ArtChannel.h): one clip per channel, named for the channel,
// SCRUBBED at t = value x duration (signed channels centred at the clip midpoint). Rigid nodes only.

// Read a FLOAT accessor's `comps`-wide entries. Returns false on any shape the spec disallows for an
// animation sampler.
static bool readAnimFloats(const tinygltf::Model& model, int accIdx, int comps, std::vector<float>& out) {
    if (accIdx < 0 || accIdx >= static_cast<int>(model.accessors.size()))
        return false;
    const tinygltf::Accessor& acc = model.accessors[static_cast<std::size_t>(accIdx)];
    if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
        return false;
    if (tinygltf::GetNumComponentsInType(static_cast<uint32_t>(acc.type)) != comps)
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

// The authored rest value of one TRS component, with glTF's defaults.
static void restValue(const tinygltf::Node& n, const std::string& path, int comps, float* out) {
    if (path == "translation") {
        for (int i = 0; i < 3; ++i)
            out[i] = (n.translation.size() == 3) ? static_cast<float>(n.translation[static_cast<std::size_t>(i)]) : 0.f;
    } else if (path == "scale") {
        for (int i = 0; i < 3; ++i)
            out[i] = (n.scale.size() == 3) ? static_cast<float>(n.scale[static_cast<std::size_t>(i)]) : 1.f;
    } else { // rotation
        const float identity[4] = {0.f, 0.f, 0.f, 1.f};
        for (int i = 0; i < 4; ++i)
            out[i] =
                (n.rotation.size() == 4) ? static_cast<float>(n.rotation[static_cast<std::size_t>(i)]) : identity[i];
    }
    (void)comps;
}

static std::string validChannelList() {
    std::string names;
    for (std::size_t i = 0; i < fl::kArtChannelCount; ++i) {
        names += (i == 0 ? " " : ", ");
        names += std::string(fl::artChannelName(static_cast<fl::ArtChannel>(i)));
    }
    return names;
}

static void checkAnimations(const tinygltf::Model& model, const std::string& label, MeshValidationResult& r) {
    // A mesh with zero animations stays valid, forever — that is the static-mesh baseline, and every
    // shipped asset is one today.
    if (model.animations.empty() && model.skins.empty())
        return;

    // Rigid mechanical parts only: a skin means the author reached for joint deformation, which the
    // engine does not implement and will refuse to load rather than half-render.
    if (!model.skins.empty()) {
        r.errors.push_back(label + ": mesh declares a skin — articulation is rigid nodes only "
                                   "(no joint skinning); the engine refuses to load it");
        r.ok = false;
    }

    // A node may be driven by at most ONE scrubbed clip, plus optionally one gear_compress_* clip:
    // deploy and compression compose only because a compression clip animates the inner strut/oleo
    // nodes, disjoint from the doors and linkages the deploy clip drives.
    std::vector<std::string> scrubbedOwner(model.nodes.size());
    std::vector<std::string> compressOwner(model.nodes.size());

    for (const tinygltf::Animation& anim : model.animations) {
        const fl::ArtChannel channel = fl::artChannelFromName(anim.name);
        if (channel == fl::ArtChannel::kCount) {
            r.warnings.push_back(
                label + ": animation \"" + anim.name +
                "\" is not a known articulation channel — it will never play. Valid:" + validChannelList());
            continue;
        }
        const bool isCompress =
            (channel == fl::ArtChannel::GearCompressNose || channel == fl::ArtChannel::GearCompressLeft ||
             channel == fl::ArtChannel::GearCompressRight);
        const bool isSigned = fl::artChannelIsSigned(channel);

        for (const tinygltf::AnimationChannel& ch : anim.channels) {
            if (ch.target_path == "weights") {
                r.errors.push_back(label + ": clip \"" + anim.name +
                                   "\" animates morph-target weights — articulation is rigid nodes only");
                r.ok = false;
                continue;
            }
            if (ch.target_node < 0 || ch.target_node >= static_cast<int>(model.nodes.size()))
                continue;
            if (ch.sampler < 0 || ch.sampler >= static_cast<int>(anim.samplers.size()))
                continue;
            const tinygltf::Node& node = model.nodes[static_cast<std::size_t>(ch.target_node)];
            const tinygltf::AnimationSampler& gs = anim.samplers[static_cast<std::size_t>(ch.sampler)];

            if (gs.interpolation != "LINEAR" && gs.interpolation != "STEP" && gs.interpolation != "CUBICSPLINE") {
                r.errors.push_back(label + ": clip \"" + anim.name + "\" uses interpolation \"" + gs.interpolation +
                                   "\" — only LINEAR, STEP and CUBICSPLINE are supported");
                r.ok = false;
                continue;
            }

            // A "_b" damage node inherits its base node's sampled pose, so animating one directly is
            // redundant work that the engine will overwrite.
            if (node.name.size() > 2 && node.name.compare(node.name.size() - 2, 2, "_b") == 0)
                r.warnings.push_back(label + ": clip \"" + anim.name + "\" targets damage node \"" + node.name +
                                     "\" directly — \"_b\" nodes inherit their base node's pose");

            // One scrubbed clip per node (plus one gear_compress_*).
            std::string& owner = isCompress ? compressOwner[static_cast<std::size_t>(ch.target_node)]
                                            : scrubbedOwner[static_cast<std::size_t>(ch.target_node)];
            if (!owner.empty() && owner != anim.name) {
                r.errors.push_back(label + ": node \"" + node.name + "\" is driven by two " +
                                   (isCompress ? "gear-compression" : "scrubbed") + " clips (\"" + owner + "\" and \"" +
                                   anim.name + "\") — they would fight over its pose");
                r.ok = false;
            }
            owner = anim.name;

            // Rest pose must equal the pose at the channel's NEUTRAL scrub time (t=0 unsigned, the
            // clip midpoint for a signed channel). This is the single most likely authoring mistake
            // and it renders the aircraft subtly wrong before anything is even commanded.
            const int comps = (ch.target_path == "rotation") ? 4 : 3;
            std::vector<float> times, values;
            if (!readAnimFloats(model, gs.input, 1, times) || !readAnimFloats(model, gs.output, comps, values) ||
                times.empty() || values.empty())
                continue; // unreadable sampler data: nothing further to say about it here
            const bool cubic = (gs.interpolation == "CUBICSPLINE");
            const std::size_t perKey = cubic ? 3u : 1u;
            const std::size_t keyCount = values.size() / (perKey * static_cast<std::size_t>(comps));
            if (keyCount == 0)
                continue;
            // Neutral keyframe: the first for an unsigned channel, the middle one for a signed one.
            const std::size_t neutralKey = isSigned ? (keyCount - 1u) / 2u : 0u;
            const std::size_t valueBase = (neutralKey * perKey + (cubic ? 1u : 0u)) * static_cast<std::size_t>(comps);
            if (valueBase + static_cast<std::size_t>(comps) > values.size())
                continue;

            float rest[4] = {0.f, 0.f, 0.f, 0.f};
            restValue(node, ch.target_path, comps, rest);
            bool mismatch = false;
            for (int i = 0; i < comps; ++i)
                if (std::abs(values[valueBase + static_cast<std::size_t>(i)] - rest[i]) > 1e-3f)
                    mismatch = true;
            if (mismatch) {
                r.errors.push_back(label + ": node \"" + node.name + "\" rest " + ch.target_path +
                                   " disagrees with clip \"" + anim.name + "\" at its neutral keyframe" +
                                   (isSigned ? " (the clip MIDPOINT, for a signed channel)" : " (t=0)") +
                                   " — the aircraft would render wrong before anything is commanded");
                r.ok = false;
            }
        }
    }
}

// ── core convention checks ────────────────────────────────────────────────────

static void applyConventionChecks(const tinygltf::Model& model, const std::string& label, MeshValidationResult& r) {
    // glTF version
    if (model.asset.version != "2.0") {
        r.errors.push_back(label + ": asset.version must be \"2.0\" (got \"" + model.asset.version + "\")");
        r.ok = false;
    }

    // At least one mesh
    if (model.meshes.empty()) {
        r.errors.push_back(label + ": no meshes found");
        r.ok = false;
    }

    // No embedded image data
    if (hasEmbeddedImages(model)) {
        r.errors.push_back(label + ": embedded image data detected — textures must be "
                                   "external .ktx2 URI references (see docs/modding/textures.md)");
        r.ok = false;
    }

    // Node naming and damage-state cross-references
    std::vector<std::string> nodeNames;
    nodeNames.reserve(model.nodes.size());
    for (const auto& node : model.nodes)
        nodeNames.push_back(node.name);

    for (const auto& node : model.nodes) {
        if (node.name.empty()) {
            r.errors.push_back(label + ": a node has an empty name");
            r.ok = false;
            continue;
        }
        if (!isLowercaseUnderscored(node.name)) {
            r.errors.push_back(label + ": node \"" + node.name +
                               "\" must be lowercase with underscores (no spaces or uppercase)");
            r.ok = false;
        }
        // Damage-state check: if name ends in "_b", base node must also exist
        if (node.name.size() > 2 && node.name[node.name.size() - 2] == '_' && node.name[node.name.size() - 1] == 'b') {
            std::string baseName = node.name.substr(0, node.name.size() - 2);
            bool baseFound = std::find(nodeNames.begin(), nodeNames.end(), baseName) != nodeNames.end();
            if (!baseFound) {
                r.errors.push_back(label + ": damage-state node \"" + node.name +
                                   "\" has no corresponding base node \"" + baseName + "\"");
                r.ok = false;
            }
        }
    }

    // Material checks
    for (const auto& mat : model.materials) {
        if (!mat.name.empty() && !isLowercaseUnderscored(mat.name)) {
            r.errors.push_back(label + ": material \"" + mat.name + "\" must be lowercase with underscores");
            r.ok = false;
        }
        for (const auto& ext : mat.extensions) {
            r.warnings.push_back(label + ": material \"" + mat.name + "\" uses extension \"" + ext.first +
                                 "\" — renderer support not guaranteed");
        }
    }

    // Winding consistency (catch inside-out meshes — flipped normals).
    checkWindingConsistency(model, label, r);

    // Articulation clips (#844). A .glb with a misspelled clip name, a skinned mesh, or a rest pose
    // that disagrees with its own t=0 keyframe used to pass clean and then simply not move in the
    // game, with no diagnostic anywhere.
    checkAnimations(model, label, r);

    // Texture URI convention (#833): the engine resolves an image URI to a Texture asset name under
    // the pack's `textures/` directory, and only .ktx2 (preferred) / .png decode. The authored form
    // is an external URI such as "../../textures/f5e_diffuse.ktx2"; flag any other extension so a URI
    // that will never load is caught here rather than rendering as a silent grey placeholder.
    for (const auto& img : model.images) {
        if (img.uri.empty() || img.uri.rfind("data:", 0) == 0)
            continue; // no URI, or an embedded data: URI (already flagged as embedded image data)
        std::string ext;
        if (auto dot = img.uri.find_last_of('.'); dot != std::string::npos)
            ext = img.uri.substr(dot);
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".ktx2" && ext != ".png")
            r.warnings.push_back(label + ": image URI \"" + img.uri +
                                 "\" is not a .ktx2 or .png reference — the engine will not load it "
                                 "(see docs/modding/textures.md)");
    }

    // Unknown extensions at model level
    for (const auto& ext : model.extensionsUsed) {
        // Known-harmless extensions (informational only). KHR_texture_basisu is REQUIRED to reference
        // a KTX2/Basis texture the spec-conformant way, so a correctly authored textured mesh must be
        // able to declare it without failing validation (#833).
        static const char* kKnownExts[] = {"KHR_materials_emissive_strength", "KHR_texture_transform",
                                           "KHR_texture_basisu", "EXT_mesh_gpu_instancing"};
        bool known = false;
        for (const char* k : kKnownExts)
            if (ext == k) {
                known = true;
                break;
            }
        if (!known) {
            r.errors.push_back(label + ": unknown glTF extension \"" + ext + "\"");
            r.ok = false;
        }
    }
}

// ── LOD sibling discovery ─────────────────────────────────────────────────────

static std::vector<fs::path> findLodSiblings(const fs::path& basePath) {
    std::vector<fs::path> lods;
    fs::path parent = basePath.parent_path();
    std::string stem = basePath.stem().string();
    std::string ext = basePath.extension().string();

    std::error_code ec;
    for (auto& entry : fs::directory_iterator(parent, ec)) {
        if (!entry.is_regular_file())
            continue;
        std::string s = entry.path().stem().string();
        std::string e = entry.path().extension().string();
        // Must share the same extension and start with "<stem>_lod"
        if (e != ext)
            continue;
        std::string prefix = stem + "_lod";
        if (s.size() <= prefix.size())
            continue;
        if (s.substr(0, prefix.size()) != prefix)
            continue;
        // Suffix after prefix must be digits
        bool allDigits = true;
        for (char c : s.substr(prefix.size()))
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                allDigits = false;
                break;
            }
        if (allDigits)
            lods.push_back(entry.path());
    }
    std::sort(lods.begin(), lods.end());
    return lods;
}

// ── public API ────────────────────────────────────────────────────────────────

MeshValidationResult validateMeshFromJson(std::string_view jsonContent) {
    MeshValidationResult r;
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;

    bool loaded = loader.LoadASCIIFromString(&model, &err, &warn, jsonContent.data(),
                                             static_cast<unsigned int>(jsonContent.size()),
                                             /*base_dir=*/"");
    if (!warn.empty())
        r.warnings.push_back("tinygltf: " + warn);
    if (!loaded) {
        r.errors.push_back("parse error: " + err);
        r.ok = false;
        return r;
    }

    applyConventionChecks(model, "<json>", r);
    return r;
}

MeshValidationResult validateMesh(const std::string& filePath) {
    MeshValidationResult r;

    fs::path p(filePath);
    std::error_code ec;
    auto size = fs::file_size(p, ec);
    if (!ec && size > kWarnFileSizeBytes) {
        r.warnings.push_back(filePath + ": file size " + std::to_string(size / (1024 * 1024)) +
                             " MB exceeds 50 MB — likely contains embedded textures");
    }

    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;
    bool loaded = false;

    std::string ext = p.extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == ".glb")
        loaded = loader.LoadBinaryFromFile(&model, &err, &warn, filePath);
    else
        loaded = loader.LoadASCIIFromFile(&model, &err, &warn, filePath);

    if (!warn.empty())
        r.warnings.push_back(filePath + ": " + warn);
    if (!loaded) {
        r.errors.push_back(filePath + ": parse error: " + err);
        r.ok = false;
        return r;
    }

    applyConventionChecks(model, filePath, r);

    // Validate LOD siblings
    for (const auto& lodPath : findLodSiblings(p)) {
        tinygltf::Model lodModel;
        std::string lodErr, lodWarn;
        bool lodLoaded = false;
        std::string lodExt = lodPath.extension().string();
        for (char& c : lodExt)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (lodExt == ".glb")
            lodLoaded = loader.LoadBinaryFromFile(&lodModel, &lodErr, &lodWarn, lodPath.string());
        else
            lodLoaded = loader.LoadASCIIFromFile(&lodModel, &lodErr, &lodWarn, lodPath.string());

        if (!lodWarn.empty())
            r.warnings.push_back(lodPath.string() + ": " + lodWarn);
        if (!lodLoaded) {
            r.errors.push_back(lodPath.string() + ": parse error: " + lodErr);
            r.ok = false;
            continue;
        }
        applyConventionChecks(lodModel, lodPath.string(), r);
    }

    return r;
}

// ── Node-tree description (#838) ────────────────────────────────────────────

namespace {
// Compose a node's local TRS into a column-major 4x4 (matrix wins if present, else T*R*S), into
// out[16]. Pure arithmetic — no glm dependency in this lib.
void composeLocalMatrix(const tinygltf::Node& node, float out[16]) {
    // Identity.
    for (int i = 0; i < 16; ++i)
        out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    if (node.matrix.size() == 16) {
        for (int i = 0; i < 16; ++i)
            out[i] = static_cast<float>(node.matrix[i]);
        return;
    }
    const double tx = node.translation.size() == 3 ? node.translation[0] : 0.0;
    const double ty = node.translation.size() == 3 ? node.translation[1] : 0.0;
    const double tz = node.translation.size() == 3 ? node.translation[2] : 0.0;
    const double qx = node.rotation.size() == 4 ? node.rotation[0] : 0.0;
    const double qy = node.rotation.size() == 4 ? node.rotation[1] : 0.0;
    const double qz = node.rotation.size() == 4 ? node.rotation[2] : 0.0;
    const double qw = node.rotation.size() == 4 ? node.rotation[3] : 1.0;
    const double sx = node.scale.size() == 3 ? node.scale[0] : 1.0;
    const double sy = node.scale.size() == 3 ? node.scale[1] : 1.0;
    const double sz = node.scale.size() == 3 ? node.scale[2] : 1.0;
    // R (column-major) from the quaternion, scaled per axis.
    const double xx = qx * qx, yy = qy * qy, zz = qz * qz;
    const double xy = qx * qy, xz = qx * qz, yz = qy * qz;
    const double wx = qw * qx, wy = qw * qy, wz = qw * qz;
    out[0] = static_cast<float>((1 - 2 * (yy + zz)) * sx);
    out[1] = static_cast<float>((2 * (xy + wz)) * sx);
    out[2] = static_cast<float>((2 * (xz - wy)) * sx);
    out[4] = static_cast<float>((2 * (xy - wz)) * sy);
    out[5] = static_cast<float>((1 - 2 * (xx + zz)) * sy);
    out[6] = static_cast<float>((2 * (yz + wx)) * sy);
    out[8] = static_cast<float>((2 * (xz + wy)) * sz);
    out[9] = static_cast<float>((2 * (yz - wx)) * sz);
    out[10] = static_cast<float>((1 - 2 * (xx + yy)) * sz);
    out[12] = static_cast<float>(tx);
    out[13] = static_cast<float>(ty);
    out[14] = static_cast<float>(tz);
}

MeshNodeTree buildNodeTree(const tinygltf::Model& model) {
    MeshNodeTree tree;
    tree.meshCount = static_cast<int>(model.meshes.size());
    tree.nodes.resize(model.nodes.size());
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const tinygltf::Node& node = model.nodes[i];
        MeshNodeInfo& info = tree.nodes[i];
        info.name = node.name;
        info.meshIndex = node.mesh;
        info.damageVariant = node.name.size() >= 2 && node.name.compare(node.name.size() - 2, 2, "_b") == 0;
        info.engineDrawn = node.mesh >= 0; // the engine draws every mesh-bearing node's primitives (#839)
        // Variant node-set tags (#882): `extras: {"fl_variant": "two_seat"}` or an array of tags.
        if (node.extras.IsObject() && node.extras.Has("fl_variant")) {
            const tinygltf::Value& tag = node.extras.Get("fl_variant");
            if (tag.IsString())
                info.variantTags.push_back(tag.Get<std::string>());
            else if (tag.IsArray())
                for (size_t t = 0; t < tag.ArrayLen(); ++t)
                    if (const tinygltf::Value& e = tag.Get(static_cast<int>(t)); e.IsString())
                        info.variantTags.push_back(e.Get<std::string>());
        }
        composeLocalMatrix(node, info.localMatrix);
        if (node.mesh >= 0 && node.mesh < static_cast<int>(model.meshes.size())) {
            const tinygltf::Mesh& m = model.meshes[node.mesh];
            info.primitiveCount = static_cast<int>(m.primitives.size());
            tree.totalPrimitives += info.primitiveCount;
            // Node-local AABB = union of its primitives' POSITION accessor min/max.
            for (const auto& prim : m.primitives) {
                auto it = prim.attributes.find("POSITION");
                if (it == prim.attributes.end() || it->second < 0 ||
                    it->second >= static_cast<int>(model.accessors.size()))
                    continue;
                const tinygltf::Accessor& acc = model.accessors[it->second];
                if (acc.minValues.size() != 3 || acc.maxValues.size() != 3)
                    continue;
                for (int k = 0; k < 3; ++k) {
                    const float lo = static_cast<float>(acc.minValues[k]);
                    const float hi = static_cast<float>(acc.maxValues[k]);
                    if (!info.hasAabb) {
                        info.aabbMin[k] = lo;
                        info.aabbMax[k] = hi;
                    } else {
                        info.aabbMin[k] = std::min(info.aabbMin[k], lo);
                        info.aabbMax[k] = std::max(info.aabbMax[k], hi);
                    }
                }
                info.hasAabb = true;
            }
        }
    }
    // Parent links from each node's children list.
    for (size_t i = 0; i < model.nodes.size(); ++i)
        for (int child : model.nodes[i].children)
            if (child >= 0 && child < static_cast<int>(tree.nodes.size()))
                tree.nodes[child].parent = static_cast<int>(i);
    return tree;
}
} // namespace

std::vector<std::string> meshVariantTags(const MeshNodeTree& tree) {
    std::vector<std::string> tags;
    for (const MeshNodeInfo& n : tree.nodes)
        for (const std::string& t : n.variantTags)
            tags.push_back(t);
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    return tags;
}

std::optional<MeshNodeTree> describeMeshNodesFromJson(std::string_view jsonContent) {
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;
    if (!loader.LoadASCIIFromString(&model, &err, &warn, jsonContent.data(),
                                    static_cast<unsigned int>(jsonContent.size()), ""))
        return std::nullopt;
    return buildNodeTree(model);
}

std::optional<MeshNodeTree> describeMeshNodesFromMemory(const uint8_t* glb, size_t len) {
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;
    if (!loader.LoadBinaryFromMemory(&model, &err, &warn, glb, static_cast<unsigned int>(len)))
        return std::nullopt;
    return buildNodeTree(model);
}

std::optional<MeshNodeTree> describeMeshNodes(const std::string& filePath) {
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;
    fs::path p(filePath);
    std::string ext = p.extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool loaded = (ext == ".glb") ? loader.LoadBinaryFromFile(&model, &err, &warn, filePath)
                                        : loader.LoadASCIIFromFile(&model, &err, &warn, filePath);
    if (!loaded)
        return std::nullopt;
    return buildNodeTree(model);
}

} // namespace fl
