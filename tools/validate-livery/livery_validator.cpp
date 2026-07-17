// SPDX-License-Identifier: GPL-3.0-or-later
#include "livery_validator.h"

#include "content/AssetManager.h"
#include "content/ContentIndex.h"
#include "content/FolderContentPack.h"
#include "content/LiveryDef.h"

#include <ILogger.h>
#include <stdfs/StdFilesystem.h>

#include <toml++/toml.hpp>

#include <array>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fl {

namespace {

namespace fs = std::filesystem;

struct SilentLogger : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// The renderer only overrides these material maps (SceneRenderer::liveryKeyFromBaseAsset). A livery
// key with any other map is legal TOML but is silently ignored at render time, so warn.
[[nodiscard]] bool isKnownMap(std::string_view map) {
    return map == "diffuse" || map == "orm" || map == "normal";
}

void checkPlausibility(const LiveryDef& def, const std::string& label, LiveryValidationResult& r) {
    if (def.aircraft.find(':') == std::string::npos)
        r.warnings.push_back(label + ": aircraft = \"" + def.aircraft +
                             "\" is not a namespaced def id (expected \"<namespace>:<id>\", e.g. "
                             "\"fl-base:f5e\") — a livery targets a def id, never a filename");
    if (def.textures.empty())
        r.warnings.push_back(label + ": [textures] overrides nothing — this livery is a no-op that renders "
                                     "the base scheme");
    for (const auto& [key, asset] : def.textures) {
        const auto dot = key.rfind('.');
        const std::string map = dot == std::string::npos ? std::string{} : key.substr(dot + 1);
        if (!isKnownMap(map))
            r.warnings.push_back(label + ": override \"" + key +
                                 "\" uses an unknown map — the renderer only skins diffuse/normal/orm, so this "
                                 "override will be ignored");
    }
}

[[nodiscard]] std::string readFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct PackManifest {
    FolderContentPack::Manifest manifest;
    bool present{false};
};

[[nodiscard]] PackManifest readManifest(const fs::path& root, LiveryValidationResult& r) {
    PackManifest out;
    out.manifest.id = "pack-under-validation";
    out.manifest.name = out.manifest.id;
    out.manifest.namespaceId = out.manifest.id;

    const fs::path path = root / "manifest.toml";
    if (!fs::exists(path)) {
        r.warnings.push_back("no manifest.toml — the engine will not mount this directory as a pack; "
                             "validating its files anyway");
        return out;
    }
    try {
        toml::table tbl = toml::parse(readFile(path));
        if (auto* mod = tbl["mod"].as_table()) {
            out.manifest.id = (*mod)["id"].value<std::string>().value_or(out.manifest.id);
            out.manifest.name = (*mod)["name"].value<std::string>().value_or(out.manifest.id);
            out.manifest.namespaceId = (*mod)["namespace"].value<std::string>().value_or(out.manifest.id);
            out.present = true;
        } else {
            r.warnings.push_back("manifest.toml has no [mod] table — the engine will refuse to mount this pack");
        }
    } catch (const toml::parse_error& e) {
        r.warnings.push_back("manifest.toml does not parse: " + std::string(e.description()));
    }
    return out;
}

} // namespace

LiveryValidationResult validateLivery(std::string_view tomlContent) {
    LiveryValidationResult r;
    LiveryDef def;
    try {
        def = parseLiveryDef(tomlContent);
    } catch (const std::exception& e) {
        r.errors.emplace_back(e.what());
        r.ok = false;
        return r;
    }
    checkPlausibility(def, def.name.empty() ? "livery" : def.name, r);
    return r;
}

LiveryValidationResult validateLiveryPack(const std::string& packDir) {
    LiveryValidationResult r;

    const fs::path root(packDir);
    if (!fs::is_directory(root)) {
        r.errors.push_back("not a directory: " + packDir);
        r.ok = false;
        return r;
    }

    const PackManifest pm = readManifest(root, r);

    // Resolve references through the REAL content system (same rule as validate-entity --pack): the
    // path rules live in FolderContentPack/AssetManager, so reimplementing them here would let the
    // tool and the engine drift, which is the failure a validator exists to prevent.
    static SilentLogger silent;
    StdFilesystem stdfs(root, root);
    auto folderPack = std::make_unique<FolderContentPack>(stdfs, silent, ".", pm.manifest);
    FolderContentPack* pack = folderPack.get();
    pack->init();

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(folderPack));
    AssetManager assets(std::move(packs), silent);

    ContentIndex index;
    constexpr std::array<AssetType, 1> kDefTypes{AssetType::EntityDef};
    index.build(assets, kDefTypes, silent);

    const std::vector<std::string> liveryNames = assets.listAssets(AssetType::Livery);
    if (liveryNames.empty())
        return r; // a pack with no liveries has nothing to validate — not an error

    for (const std::string& name : liveryNames) {
        const std::string file = "liveries/" + name + ".toml";
        const auto data = pack->loadLivery(name.c_str());
        if (!data) {
            r.errors.push_back(file + ": listed but unreadable");
            r.ok = false;
            continue;
        }

        LiveryDef def;
        try {
            def =
                parseLiveryDef(std::string_view(reinterpret_cast<const char*>(data->bytes.data()), data->bytes.size()));
        } catch (const std::exception& e) {
            r.errors.push_back(file + ": " + e.what());
            r.ok = false;
            continue;
        }
        def.id = name;

        checkPlausibility(def, file, r);

        // The aircraft DEF ID: resolved through the index like every def-id reference (#810). A livery
        // pack legitimately targets an aircraft that lives in a BASE pack, so a local miss is a
        // WARNING, not an error — the engine resolves it against the full mounted stack.
        if (index.assetNameFor(AssetType::EntityDef, def.aircraft) == nullptr)
            r.warnings.push_back(file + ": aircraft \"" + def.aircraft +
                                 "\" is not an entity in THIS pack — fine if it lives in a base pack, but "
                                 "the livery does nothing without it");

        // Texture asset names: files the livery pack must actually ship (it carries its own skins).
        for (const auto& [key, asset] : def.textures) {
            if (!pack->hasAsset(asset.c_str(), AssetType::Texture)) {
                r.errors.push_back(file + ": " + key + " = \"" + asset +
                                   "\" does not resolve to a texture file in this pack");
                r.ok = false;
            }
        }
    }

    return r;
}

} // namespace fl
