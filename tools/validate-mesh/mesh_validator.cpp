// SPDX-License-Identifier: GPL-3.0-or-later

// tinygltf implementation defines — must appear in exactly one .cpp file.
// TINYGLTF_NO_STB_IMAGE_WRITE: we only load/validate models, never write images.
// Skipping stb_image_write.h eliminates -Wmissing-field-initializers in that header.
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define STB_IMAGE_IMPLEMENTATION
#include <tiny_gltf.h>

#include "mesh_validator.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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

    // Unknown extensions at model level
    for (const auto& ext : model.extensionsUsed) {
        // Known-harmless extensions (informational only)
        static const char* kKnownExts[] = {"KHR_materials_emissive_strength", "KHR_texture_transform",
                                           "EXT_mesh_gpu_instancing"};
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
