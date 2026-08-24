// SPDX-License-Identifier: GPL-3.0-or-later
#include "campaign_validator.h"

#include "ILogger.h"
#include "NullLogger.h"
#include "campaign/CampaignParser.h"
#include "campaign/FrontlinePng.h"
#include "campaign/TemplateHeader.h"
#include "campaign/TheaterManifest.h"
#include "content/AssetManager.h"
#include "content/FolderContentPack.h"

#include <stdfs/StdFilesystem.h>
#include <toml++/toml.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace fl {

namespace {
// Build a lenient FolderContentPack over the pack dir (id fields don't matter for our lookups).
FolderContentPack::Manifest lenientManifest(const fs::path& root) {
    FolderContentPack::Manifest m;
    m.id = "pack-under-validation";
    m.name = m.id;
    m.namespaceId = m.id;
    if (fs::exists(root / "manifest.toml")) {
        try {
            toml::table tbl = toml::parse(readFileBinary(root / "manifest.toml"));
            if (auto* mod = tbl["mod"].as_table()) {
                m.id = (*mod)["id"].value<std::string>().value_or(m.id);
                m.namespaceId = (*mod)["namespace"].value<std::string>().value_or(m.id);
            }
        } catch (const toml::parse_error&) {
        }
    }
    return m;
}

// Collect every distinct ${name...} placeholder's leading NAME token from a template body.
std::set<std::string> placeholderNames(std::string_view body) {
    std::set<std::string> names;
    size_t pos = 0;
    while ((pos = body.find("${", pos)) != std::string_view::npos) {
        size_t end = body.find('}', pos + 2);
        if (end == std::string_view::npos)
            break;
        std::string_view inner = body.substr(pos + 2, end - (pos + 2));
        size_t dot = inner.find('.');
        names.insert(std::string(inner.substr(0, dot == std::string_view::npos ? inner.size() : dot)));
        pos = end + 1;
    }
    return names;
}
} // namespace

PackYamlKind classifyPackYaml(std::string_view yaml) {
    // Column-0 key scan (mirrors stripTemplateHeader's discipline): a `template:` header => Template;
    // a `dynamic:` or `story:` top-level key => Campaign; otherwise a plain Mission.
    size_t pos = 0;
    bool hasCampaignKey = false;
    while (pos <= yaml.size()) {
        size_t nl = yaml.find('\n', pos);
        std::string_view line = yaml.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        if (!line.empty() && line[0] != ' ' && line[0] != '\t' && line[0] != '#') {
            if (line.rfind("template:", 0) == 0)
                return PackYamlKind::Template;
            if (line.rfind("dynamic:", 0) == 0 || line.rfind("story:", 0) == 0)
                hasCampaignKey = true;
        }
        if (nl == std::string_view::npos)
            break;
        pos = nl + 1;
    }
    return hasCampaignKey ? PackYamlKind::Campaign : PackYamlKind::Mission;
}

CampaignValidationResult validateCampaign(std::string_view yamlContent) {
    CampaignValidationResult r;
    CampaignParseResult p = parseCampaign(yamlContent);
    r.errors = p.errors;
    r.warnings = p.warnings;
    r.ok = p.ok;
    return r;
}

CampaignValidationResult validateCampaign(std::string_view yamlContent, const std::string& packDir) {
    CampaignValidationResult r = validateCampaign(yamlContent);
    if (packDir.empty())
        return r;

    const fs::path root(packDir);
    if (!fs::is_directory(root)) {
        r.errors.push_back("not a directory: " + packDir);
        r.ok = false;
        return r;
    }
    // Re-parse for the def (validateCampaign above only kept errors/warnings).
    CampaignParseResult p = parseCampaign(yamlContent);
    const CampaignDef& def = p.campaign;

    // Silent by design: the content system's own log lines would duplicate the findings this tool
    // reports itself, and break the "clean validation prints nothing" contract CI relies on.
    static NullLogger silent;
    StdFilesystem stdfs(root, root);
    auto folderPack = std::make_unique<FolderContentPack>(stdfs, silent, ".", lenientManifest(root));
    FolderContentPack* pack = folderPack.get();
    pack->init();
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(folderPack));
    AssetManager assets(std::move(packs), silent);

    // Resolve a pack-relative file: builtin? loadPackFile? Mission-asset stem? (mirror runtime).
    auto fileResolves = [&](const std::string& path) -> bool {
        if (assets.loadPackFile(path.c_str()))
            return true;
        // Mission-asset stem fallback (missions/<stem>.yaml): strip a leading "missions/" + ".yaml".
        std::string stem = path;
        if (stem.rfind("missions/", 0) == 0)
            stem = stem.substr(9);
        if (auto dot = stem.rfind(".yaml"); dot != std::string::npos)
            stem = stem.substr(0, dot);
        return assets.loadMission(stem.c_str()) != nullptr;
    };

    // Per-theater: manifest + bounds; index by id for frontline grid lookup.
    std::set<std::string> theaterIds;
    for (const auto& th : def.theaters) {
        theaterIds.insert(th.id);
        auto tf = assets.loadTheater(th.id.c_str());
        if (!tf || tf->bytes.empty()) {
            r.errors.push_back("theater '" + th.id + "': no theaters/" + th.id + ".toml manifest in the pack");
            continue;
        }
        auto tr =
            parseTheaterManifest(std::string_view(reinterpret_cast<const char*>(tf->bytes.data()), tf->bytes.size()));
        for (const auto& e : tr.errors)
            r.errors.push_back("theater '" + th.id + "': " + e);
        for (const auto& w : tr.warnings)
            r.warnings.push_back("theater '" + th.id + "': " + w);
        // A theater legitimately rides fl-base's world terrain; an unresolved terrain id is a warning.
        if (tr.ok && tr.theater.terrain != "world" && assets.loadTerrain(tr.theater.terrain.c_str()) == nullptr)
            r.warnings.push_back("theater '" + th.id + "': terrain '" + tr.theater.terrain + "' not found in the pack");

        // Frontline rasters (initial + story set_frontline) that belong to this theater's grid.
        auto checkRaster = [&](const std::string& relPath, const char* what) {
            if (relPath.empty())
                return;
            auto bytes = assets.loadPackFile(relPath.c_str());
            if (!bytes) {
                r.errors.push_back(std::string(what) + " '" + relPath + "' not found in the pack");
                return;
            }
            FrontlinePngInfo info = probeFrontlinePng(bytes->data(), bytes->size());
            if (!info.ok) {
                r.errors.push_back(std::string(what) + " '" + relPath + "': " + info.error);
                return;
            }
            if (info.width != th.frontlineCols || info.height != th.frontlineRows) {
                r.errors.push_back(std::string(what) + " '" + relPath + "' is " + std::to_string(info.width) + "x" +
                                   std::to_string(info.height) + " but theater '" + th.id +
                                   "' declares frontline_grid " + std::to_string(th.frontlineCols) + "x" +
                                   std::to_string(th.frontlineRows));
            }
        };
        checkRaster(th.initialFrontline, "initial_frontline");
    }

    // Story frontlines apply to the story's own theater's grid.
    for (const auto& s : def.story) {
        const CampaignTheater* th = nullptr;
        for (const auto& t : def.theaters)
            if (t.id == s.theaterId)
                th = &t;
        auto checkStoryRaster = [&](const std::string& relPath, const char* what) {
            if (relPath.empty())
                return;
            auto bytes = assets.loadPackFile(relPath.c_str());
            if (!bytes) {
                r.errors.push_back("story '" + s.id + "': " + what + " '" + relPath + "' not found in the pack");
                return;
            }
            FrontlinePngInfo info = probeFrontlinePng(bytes->data(), bytes->size());
            if (!info.ok) {
                r.errors.push_back("story '" + s.id + "': " + what + " '" + relPath + "': " + info.error);
                return;
            }
            if (th && (info.width != th->frontlineCols || info.height != th->frontlineRows))
                r.errors.push_back("story '" + s.id + "': " + what + " '" + relPath + "' is " +
                                   std::to_string(info.width) + "x" + std::to_string(info.height) + " but theater '" +
                                   th->id + "' declares frontline_grid " + std::to_string(th->frontlineCols) + "x" +
                                   std::to_string(th->frontlineRows));
        };
        checkStoryRaster(s.onComplete.setFrontline, "on_complete.set_frontline");
        checkStoryRaster(s.onFail.setFrontline, "on_fail.set_frontline");

        if (!s.file.empty() && !fileResolves(s.file))
            r.errors.push_back("story '" + s.id + "': file '" + s.file + "' does not resolve in the pack");
    }

    // Templates: file resolves; header role + fills sources; placeholders reference declared fills.
    for (const auto& th : def.theaters) {
        for (const auto& tpl : th.templates) {
            if (tpl.file.empty()) {
                r.errors.push_back("theater '" + th.id + "': a template has no file");
                continue;
            }
            auto bytes = assets.loadPackFile(tpl.file.c_str());
            if (!bytes) {
                r.errors.push_back("theater '" + th.id + "': template file '" + tpl.file + "' does not resolve");
                continue;
            }
            std::string body(reinterpret_cast<const char*>(bytes->data()), bytes->size());
            TemplateHeaderResult hdr = parseTemplateHeader(body);
            for (const auto& e : hdr.errors)
                r.errors.push_back("template '" + tpl.file + "': " + e);
            for (const auto& w : hdr.warnings)
                r.warnings.push_back("template '" + tpl.file + "': " + w);
            if (!hdr.present) {
                r.warnings.push_back("template '" + tpl.file + "': no template: header");
                continue;
            }
            if (hdr.role.empty())
                r.warnings.push_back("template '" + tpl.file + "': header declares no role");
            std::set<std::string> declared;
            for (const auto& f : hdr.fills) {
                declared.insert(f.name);
                if (!f.from.empty() && f.from != "frontline" && f.from != "ground_units")
                    r.errors.push_back("template '" + tpl.file + "': fill '" + f.name + "' has unknown from: '" +
                                       f.from + "' (frontline|ground_units)");
            }
            for (const std::string& ph : placeholderNames(body))
                if (declared.count(ph) == 0)
                    r.errors.push_back("template '" + tpl.file + "': ${" + ph + "...} references an undeclared fill");
        }
    }

    r.ok = r.errors.empty();
    return r;
}

} // namespace fl
