// SPDX-License-Identifier: GPL-3.0-or-later
#include "entity_validator.h"

#include "mesh_validator.h" // #882: describeMeshNodesFromMemory / meshVariantTags for the variant cross-check

#include "ILogger.h"
#include "NullLogger.h"
#include "content/AssetManager.h"
#include "content/ContentIndex.h"
#include "content/FolderContentPack.h"
#include "entity/EntityDef.h"
#include "entity/EntityDefParser.h"
#include "sensor/BuiltinSensors.h"
#include "weapon/BuiltinWeapon.h"

#include <stdfs/StdFilesystem.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace fl {

namespace {

// AssetManager lowercases every asset name before it reaches a pack (cacheKey), so runtime
// resolution is effectively case-insensitive. Mirror that here, or a name that only differs in
// case would validate and then miss at runtime.
[[nodiscard]] std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// The two plausible wrong spellings of an asset name (#818/#829). The correct form includes the
// asset's own subdirectory but not the type directory: aircraft/f5e/f5e.toml is named "f5e/f5e".
// Returns the correction if one of the wrong forms was written and the corrected name resolves.
[[nodiscard]] std::string suggestAssetName(const IContentPack& pack, const std::string& name, AssetType type) {
    if (name.find('/') == std::string::npos) {
        // Wrong form 1: the bare stem, missing its subdirectory ("f5e" for "f5e/f5e").
        const std::string candidate = name + "/" + name;
        if (pack.hasAsset(candidate.c_str(), type))
            return candidate;
    } else {
        // Wrong form 2: the type directory pasted on the front ("aircraft/f5e/f5e"), which
        // FolderContentPack would double-prefix.
        const std::string candidate = name.substr(name.find('/') + 1);
        if (!candidate.empty() && pack.hasAsset(candidate.c_str(), type))
            return candidate;
    }
    return {};
}

void checkAssetRef(const IContentPack& pack, const std::string& file, const char* field, const std::string& rawName,
                   AssetType type, EntityValidationResult& r) {
    if (rawName.empty())
        return; // absent is legal; the silent-fallback cases warn separately

    const std::string name = lowered(rawName);
    if (pack.hasAsset(name.c_str(), type))
        return;

    std::string msg = file + ": " + field + " = \"" + rawName + "\" does not resolve to a file in this pack";
    if (const std::string fix = suggestAssetName(pack, name, type); !fix.empty())
        msg += " — did you mean \"" + fix +
               "\"? (an asset name includes its own subdirectory, "
               "but never the type directory)";
    r.errors.push_back(std::move(msg));
    r.ok = false;
}

// The fallbacks that bite silently. Neither is illegal — the zero-pack sandbox flies the builtin
// model under the builtin placeholder shapes on purpose — but a PACK aircraft that does so is
// almost always a mistake, and at runtime nobody is watching the log line that says so.
void checkSilentFallbacks(const EntityDef& def, const std::string& label, EntityValidationResult& r) {
    const bool vehicle = def.category == ObjectCategory::AirVehicle || def.category == ObjectCategory::GroundVehicle ||
                         def.category == ObjectCategory::NavalVehicle || def.category == ObjectCategory::Structure;
    if (def.category == ObjectCategory::AirVehicle && def.flightModelAsset.empty())
        r.warnings.push_back(label + ": no flight_model — this aircraft will fly the builtin placeholder model");
    if (vehicle && def.mesh.empty())
        r.warnings.push_back(label + ": no mesh — this entity will render as the builtin placeholder shape");
}

struct PackManifest {
    FolderContentPack::Manifest manifest;
    bool present{false};
};

[[nodiscard]] PackManifest readManifest(const fs::path& root, EntityValidationResult& r) {
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
        toml::table tbl = toml::parse(readFileBinary(path));
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

EntityValidationResult validateEntity(std::string_view tomlContent) {
    EntityValidationResult r;

    EntityDef def;
    try {
        def = parseEntityDef(tomlContent);
    } catch (const std::exception& e) {
        r.errors.push_back(e.what());
        r.ok = false;
        return r;
    }

    checkSilentFallbacks(def, def.id, r);
    return r;
}

EntityValidationResult validateEntityPack(const std::string& packDir) {
    EntityValidationResult r;

    const fs::path root(packDir);
    if (!fs::is_directory(root)) {
        r.errors.push_back("not a directory: " + packDir);
        r.ok = false;
        return r;
    }

    const PackManifest pm = readManifest(root, r);

    // Resolve every reference through the REAL content system. The f5e/f5e defect was a path rule
    // (#818); reimplementing path rules here would let this tool and the engine drift apart, which
    // is the exact failure mode a validator exists to prevent.
    // Silent by design: the content system's own log lines would duplicate the findings this tool
    // reports itself, and break the "clean validation prints nothing" contract CI relies on.
    static NullLogger silent;
    StdFilesystem stdfs(root, root);
    auto folderPack = std::make_unique<FolderContentPack>(stdfs, silent, ".", pm.manifest);
    FolderContentPack* pack = folderPack.get();
    pack->init();

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(folderPack));
    AssetManager assets(std::move(packs), silent);

    ContentIndex index;
    constexpr std::array<AssetType, 2> kDefTypes{AssetType::SensorDef, AssetType::Weapon};
    index.build(assets, kDefTypes, silent);

    const std::vector<std::string> entityNames = assets.listAssets(AssetType::EntityDef);
    if (entityNames.empty())
        return r; // a pack with no entities has nothing to validate — not an error

    std::set<std::string> seenIds;
    for (const std::string& name : entityNames) {
        const std::string file = "entities/" + name + ".toml";

        const auto data = pack->loadEntityDef(name.c_str());
        if (!data) {
            r.errors.push_back(file + ": listed but unreadable");
            r.ok = false;
            continue;
        }

        EntityDef def;
        try {
            def =
                parseEntityDef(std::string_view(reinterpret_cast<const char*>(data->bytes.data()), data->bytes.size()));
        } catch (const std::exception& e) {
            r.errors.push_back(file + ": " + e.what());
            r.ok = false;
            continue;
        }

        if (!seenIds.insert(def.id).second) {
            r.errors.push_back(file + ": duplicate entity id \"" + def.id + "\"");
            r.ok = false;
        }

        if (pm.present) {
            const auto colon = def.id.find(':');
            if (colon != std::string::npos && def.id.substr(0, colon) != pm.manifest.namespaceId)
                r.warnings.push_back(file + ": id prefix \"" + def.id.substr(0, colon) +
                                     "\" does not match the pack namespace \"" + pm.manifest.namespaceId +
                                     "\" — the engine warns on every load");
        }

        // Asset-name references: files the pack must actually have.
        checkAssetRef(*pack, file, "mesh", def.mesh, AssetType::Mesh, r);
        checkAssetRef(*pack, file, "cockpit", def.cockpitMesh, AssetType::Mesh, r);
        checkAssetRef(*pack, file, "classic.damage_mesh", def.classicDamageMesh, AssetType::Mesh, r);
        checkAssetRef(*pack, file, "flight_model", def.flightModelAsset, AssetType::FlightModel, r);
        checkAssetRef(*pack, file, "ai_script", def.aiScriptAsset, AssetType::AIScript, r);
        checkAssetRef(*pack, file, "manual", def.manualAsset, AssetType::Manual, r);

        // Variant node-set (#882): `mesh_variant` must match a tag some node in the referenced mesh
        // actually declares. A typo here is invisible at runtime — the aircraft simply renders as the
        // untagged shared airframe, missing exactly the geometry that made it a distinct variant.
        if (!def.meshVariant.empty()) {
            if (def.mesh.empty()) {
                r.errors.push_back(file + ": mesh_variant = \"" + def.meshVariant + "\" but no mesh is declared");
                r.ok = false;
            } else if (const auto meshData = pack->loadMesh(lowered(def.mesh).c_str()); meshData) {
                const auto tree = describeMeshNodesFromMemory(meshData->bytes.data(), meshData->bytes.size());
                const std::vector<std::string> tags = tree ? meshVariantTags(*tree) : std::vector<std::string>{};
                if (std::find(tags.begin(), tags.end(), def.meshVariant) == tags.end()) {
                    std::string msg = file + ": mesh_variant = \"" + def.meshVariant +
                                      "\" matches no fl_variant tag in mesh \"" + def.mesh +
                                      "\" — the entity would render as the untagged airframe only";
                    if (!tags.empty()) {
                        msg += " (available:";
                        for (const std::string& t : tags)
                            msg += " \"" + t + "\"";
                        msg += ")";
                    }
                    r.errors.push_back(std::move(msg));
                    r.ok = false;
                }
            }
        }

        // Def-id references: resolved through the index, never the filesystem (#810). Builtin ids
        // ("builtin:eyeball", the #440 seeker heads) are compiled in, never pack files.
        for (const std::string& sid : def.sensorIds) {
            if (sid == sensor::BuiltinSensors::eyeball().id || sid == sensor::BuiltinSensors::irSeeker().id ||
                sid == sensor::BuiltinSensors::radarSeeker().id)
                continue;
            if (index.assetNameFor(AssetType::SensorDef, sid) == nullptr) {
                r.errors.push_back(file + ": sensors id \"" + sid +
                                   "\" does not resolve to any sensor def in this pack — the entity "
                                   "would spawn without it");
                r.ok = false;
            }
        }

        for (const Hardpoint& hp : def.hardpoints) {
            auto checkWeaponId = [&](const char* field, const std::string& wid) {
                if (wid.empty() || wid == BuiltinWeapon::cannon().id || wid == BuiltinWeapon::irMissile().id ||
                    wid == BuiltinWeapon::radarMissile().id)
                    return;
                if (index.assetNameFor(AssetType::Weapon, wid) == nullptr) {
                    r.errors.push_back(file + ": hardpoint slot " + std::to_string(hp.slot) + " " + field +
                                       " references unknown weapon id \"" + wid +
                                       "\" (if the weapon file exists, is it valid? run validate-weapon)");
                    r.ok = false;
                }
            };
            for (const std::string& wid : hp.allowed)
                checkWeaponId("allowed", wid);
            checkWeaponId("default", hp.defaultWeapon);
        }

        // Crew seats (#966/#968): the one-owner-per-channel invariant and every slot/turret/
        // capability reference are single-file and already enforced by parseEntityDef above (a
        // violation surfaced as a parse error). The one crew reference that needs PACK resolution
        // is a `lua:<script>` bot spec — a Lua seat bot loaded from the pack's ai/ directory.
        // A `builtin:*` spec or a bare C++ factory behavior (gunner, guns, ...) is compiled in.
        for (const SeatDef& seat : def.crew) {
            if (seat.defaultOccupancy != SeatOccupancyDefault::Bot)
                continue;
            constexpr std::string_view kLua = "lua:";
            if (seat.botSpec.size() > kLua.size() && seat.botSpec.compare(0, kLua.size(), kLua) == 0) {
                const std::string script = seat.botSpec.substr(kLua.size());
                checkAssetRef(*pack, file, ("crew seat \"" + seat.role + "\" bot lua script").c_str(), script,
                              AssetType::AIScript, r);
            }
        }

        checkSilentFallbacks(def, file, r);
    }

    return r;
}

} // namespace fl
