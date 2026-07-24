// SPDX-License-Identifier: GPL-3.0-or-later
#include "mod_validator.h"

// Compose the per-asset validator libs (the same ones fl-base-pack CI runs) — linking, never
// subprocessing (#651). Each contributes its own {ok, errors, warnings}; we prefix + merge.
#include "campaign_validator.h"     // validateCampaign, classifyPackYaml
#include "entity_validator.h"       // validateEntityPack
#include "flight_model_validator.h" // validateFlightModel
#include "license_validator.h"      // validateLicenses
#include "livery_validator.h"       // validateLiveryPack
#include "mesh_validator.h"         // validateMesh
#include "mission_validator.h"      // validateMission
#include "mode_validator.h"         // validateGameMode
#include "playlist_validator.h"     // validatePlaylist
#include "sensor_validator.h"       // validateSensor
#include "weapon_validator.h"       // validatePackWeapons

#include "campaign/TheaterManifest.h"
#include "content/ModManifest.h"
#include "crypto/Sha256.h"
#include "world/AirportDefParser.h"

#include <toml++/toml.hpp>

#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace fl {

namespace {
std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::vector<uint8_t> readBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Merge a validator's findings with a domain prefix.
template <typename R> void merge(ModValidationResult& out, const char* domain, const R& r) {
    for (const auto& e : r.errors)
        out.errors.push_back(std::string(domain) + ": " + e);
    for (const auto& w : r.warnings)
        out.warnings.push_back(std::string(domain) + ": " + w);
}

bool isHex64(const std::string& s) {
    if (s.size() != 64)
        return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    return true;
}
std::string toLowerHex(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Iterate *.toml / *.glb / etc. under a subdir (recursively), calling fn(relPathFromRoot, absPath).
void forEachFile(const fs::path& dir, const char* ext, const std::function<void(const fs::path&)>& fn) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec)
            break;
        if (it->is_regular_file(ec) && it->path().extension() == ext)
            fn(it->path());
    }
}
} // namespace

ModValidationResult validateMod(const std::string& packDir, const ModValidateOptions& opts) {
    ModValidationResult out;
    const fs::path root(packDir);
    if (!fs::is_directory(root)) {
        out.errors.push_back("structure: not a directory: " + packDir);
        out.ok = false;
        return out;
    }

    // 1. Manifest — the engine will not mount a pack without a valid one, so a missing/invalid manifest
    //    is an ERROR (unlike validate-entity's --pack, which only warns).
    const fs::path manifestPath = root / "manifest.toml";
    ModManifestParseResult manifest;
    if (!fs::exists(manifestPath)) {
        out.errors.push_back("manifest: no manifest.toml — the engine will not mount this directory as a pack");
    } else {
        manifest = parseModManifest(readFile(manifestPath));
        merge(out, "manifest", manifest);
    }

    // 2. Optional [files] SHA-256 table (the cheap half of #246): "<relpath>" = "<sha256 hex>".
    if (fs::exists(manifestPath)) {
        try {
            toml::table tbl = toml::parse(readFile(manifestPath));
            if (auto* files = tbl["files"].as_table()) {
                for (auto&& [key, val] : *files) {
                    const std::string rel(key.str());
                    auto expected = val.value<std::string>();
                    const fs::path fp = root / rel;
                    if (!fs::exists(fp)) {
                        out.errors.push_back("files: [files] lists '" + rel + "' but it does not exist");
                        continue;
                    }
                    if (!expected || !isHex64(*expected)) {
                        out.errors.push_back("files: [files]['" + rel + "'] is not a 64-char sha256 hex");
                        continue;
                    }
                    auto bytes = readBytes(fp);
                    const std::string got = sha256Hex(bytes.data(), bytes.size());
                    if (toLowerHex(*expected) != got)
                        out.errors.push_back("files: '" + rel + "' sha256 mismatch (expected " + toLowerHex(*expected) +
                                             ", got " + got + ")");
                }
            }
        } catch (const toml::parse_error&) {
            // The manifest parse error was already reported by parseModManifest.
        }
    }

    // 3. Structure: warn on a top-level directory the engine never loads from.
    static const std::set<std::string> kKnownDirs = {
        "aircraft",   "textures",  "audio",  "missions", "terrain",  "ai",    "entities",
        "sensors",    "weapons",   "manual", "liveries", "airports", "modes", "theaters",
        "frontlines", "templates", "data",   "tools",    "scripts",  "docs",  "LICENSES"};
    {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(root, ec)) {
            if (!e.is_directory(ec))
                continue;
            const std::string name = e.path().filename().string();
            if (name.rfind(".git", 0) == 0 || name == ".github")
                continue;
            if (kKnownDirs.count(name) == 0)
                out.warnings.push_back("structure: top-level directory '" + name +
                                       "' is not a known asset dir — the engine will not load from it");
        }
    }

    // 4. Composed pack validators (each dedupes internally; they own the cross-references).
    merge(out, "entities", validateEntityPack(packDir));
    merge(out, "weapons", validatePackWeapons(packDir));
    merge(out, "liveries", validateLiveryPack(packDir));

    // 5. Per-file drives for types with no pack-mode lib API.
    forEachFile(root / "sensors", ".toml",
                [&](const fs::path& p) { merge(out, "sensors", validateSensor(readFile(p))); });
    forEachFile(root / "modes", ".toml",
                [&](const fs::path& p) { merge(out, "modes", validateGameMode(readFile(p))); });
    forEachFile(root / "aircraft", ".toml",
                [&](const fs::path& p) { merge(out, "flight-models", validateFlightModel(readFile(p))); });
    forEachFile(root / "aircraft", ".glb", [&](const fs::path& p) {
        // Skip LOD siblings (validateMesh auto-discovers them from the base file).
        const std::string stem = p.stem().string();
        if (stem.find("_lod") != std::string::npos)
            return;
        merge(out, "meshes", validateMesh(p.string()));
    });
    forEachFile(root / "airports", ".toml", [&](const fs::path& p) {
        try {
            (void)parseAirportDef(readFile(p));
        } catch (const std::exception& e) {
            out.errors.push_back("airports: " + p.filename().string() + ": " + e.what());
        }
    });
    forEachFile(root / "theaters", ".toml",
                [&](const fs::path& p) { merge(out, "theaters", parseTheaterManifest(readFile(p))); });
    if (fs::exists(root / "data" / "playlist.toml"))
        merge(out, "playlist", validatePlaylist((root / "data" / "playlist.toml").string(), packDir));

    // 6. missions/*.yaml: discriminate mission vs campaign vs template and route accordingly.
    std::set<std::string> referencedTemplates; // for the orphan-template warning
    forEachFile(root / "missions", ".yaml", [&](const fs::path& p) {
        const std::string yaml = readFile(p);
        switch (classifyPackYaml(yaml)) {
        case PackYamlKind::Mission:
            merge(out, "missions", validateMission(yaml, packDir));
            break;
        case PackYamlKind::Campaign:
            merge(out, "campaigns", validateCampaign(yaml, packDir));
            break;
        case PackYamlKind::Template:
            // Validated through the campaign that references it; here only note it exists.
            break;
        }
    });

    // 7. Licenses: run validate-licenses when the pack declares REUSE.toml.
    if (opts.checkLicenses) {
        if (fs::exists(root / "REUSE.toml")) {
            std::vector<std::string> allowed = {"CC0-1.0", "CC-BY-4.0"};
            for (const auto& a : opts.allowedSpdx)
                allowed.push_back(a);
            merge(out, "licenses", validateLicenses(packDir, allowed, (root / "LICENSES").string()));
        } else {
            out.warnings.push_back("licenses: no REUSE.toml — the pack declares no license metadata");
        }
    }

    out.ok = out.errors.empty();
    return out;
}

} // namespace fl
