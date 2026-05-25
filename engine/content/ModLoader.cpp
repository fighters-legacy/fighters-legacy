// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/ModLoader.h"

#include "IFilesystem.h"
#include "ILogger.h"
#include "content/FolderContentPack.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <unordered_set>

ModLoader::ModLoader(IFilesystem& fs, ILogger& logger) : m_fs(fs), m_logger(logger) {}

bool ModLoader::validateEngineApi(const std::string& engineApi, const std::string& modId) {
    // Check that the major version component matches kEngineApiMajor.
    // "1.0", "1.1" are compatible; "2.0" is not.
    auto dot = engineApi.find('.');
    std::string major = (dot != std::string::npos) ? engineApi.substr(0, dot) : engineApi;
    if (major != kEngineApiMajor) {
        m_logger.log(LogLevel::Error, __FILE__, __LINE__,
                     ("mod '" + modId + "': engine-api " + engineApi +
                      " is incompatible (engine major version: " + kEngineApiMajor + ")")
                         .c_str());
        return false;
    }
    return true;
}

std::optional<ModLoader::Manifest> ModLoader::parseManifest(const char* path) {
    // Read file into string
    int handle = m_fs.openFile(PathDomain::Assets, path, false);
    if (handle < 0)
        return std::nullopt; // missing manifest.toml ��� silently skip at Debug level

    std::size_t size = m_fs.getFileSize(handle);
    std::string content(size, '\0');
    m_fs.readFile(handle, content.data(), size);
    m_fs.closeFile(handle);

    toml::table tbl;
    try {
        tbl = toml::parse(content);
    } catch (const toml::parse_error& e) {
        m_logger.log(LogLevel::Error, __FILE__, __LINE__,
                     (std::string("failed to parse manifest '") + path + "': " + e.what()).c_str());
        return std::nullopt;
    }

    auto mod = tbl["mod"];
    if (!mod) {
        m_logger.log(LogLevel::Error, __FILE__, __LINE__,
                     (std::string("manifest '") + path + "': missing [mod] table").c_str());
        return std::nullopt;
    }

    // Validate required fields
    auto name = mod["name"].value<std::string>();
    auto id = mod["id"].value<std::string>();
    auto version = mod["version"].value<std::string>();
    auto engineApi = mod["engine-api"].value<std::string>();
    auto priority = mod["priority"].value<int>();

    if (!name || !id || !version || !engineApi || !priority) {
        m_logger.log(LogLevel::Error, __FILE__, __LINE__,
                     (std::string("manifest '") + path +
                      "': missing required field(s) (name, id, version, engine-api, priority)")
                         .c_str());
        return std::nullopt;
    }

    Manifest manifest;
    manifest.name = std::move(*name);
    manifest.id = std::move(*id);
    manifest.version = std::move(*version);
    manifest.engineApi = std::move(*engineApi);
    manifest.priority = *priority;

    // Optional depends array
    if (auto deps = mod["depends"].as_array()) {
        for (auto& dep : *deps) {
            if (auto s = dep.value<std::string>())
                manifest.depends.push_back(std::move(*s));
        }
    }

    return manifest;
}

std::vector<std::unique_ptr<IContentPack>> ModLoader::load() {
    m_loadErrors.clear();

    // Scan mods directory; handle absent directory gracefully
    auto entries = m_fs.scanDirectory(PathDomain::Assets, kModsDir);
    if (entries.empty()) {
        m_logger.log(LogLevel::Info, __FILE__, __LINE__, "mods directory is absent or empty — no content packs loaded");
        return {};
    }

    // Pass 1: parse all valid manifests, collect IDs for dependency validation
    struct Candidate {
        Manifest manifest;
        std::string modDir;
    };
    std::vector<Candidate> candidates;
    std::unordered_set<std::string> loadedIds;

    for (auto& entry : entries) {
        if (!entry.isDirectory)
            continue;
        std::string modDir = std::string(kModsDir) + "/" + entry.name;
        std::string manifestPath = modDir + "/manifest.toml";

        if (!m_fs.fileExists(PathDomain::Assets, manifestPath.c_str())) {
            m_logger.log(LogLevel::Debug, __FILE__, __LINE__, ("skipping '" + modDir + "': no manifest.toml").c_str());
            continue;
        }

        auto manifest = parseManifest(manifestPath.c_str());
        if (!manifest) {
            m_loadErrors.push_back({modDir, "", "failed to parse manifest"});
            continue;
        }

        if (!validateEngineApi(manifest->engineApi, manifest->id)) {
            m_loadErrors.push_back({modDir, manifest->id, "incompatible engine-api: " + manifest->engineApi});
            continue;
        }

        loadedIds.insert(manifest->id);
        candidates.push_back({std::move(*manifest), std::move(modDir)});
    }

    // Pass 2: validate dependencies
    for (auto& c : candidates) {
        for (auto& dep : c.manifest.depends) {
            if (loadedIds.find(dep) == loadedIds.end()) {
                m_logger.log(
                    LogLevel::Warn, __FILE__, __LINE__,
                    ("mod '" + c.manifest.id + "': dependency '" + dep + "' not found — mod may not function correctly")
                        .c_str());
            }
        }
    }

    // Build FolderContentPack instances
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.reserve(candidates.size());
    for (auto& c : candidates) {
        FolderContentPack::Manifest fm;
        fm.name = c.manifest.name;
        fm.id = c.manifest.id;
        fm.version = c.manifest.version;
        fm.engineApi = c.manifest.engineApi;
        fm.priority = c.manifest.priority;
        packs.push_back(std::make_unique<FolderContentPack>(m_fs, m_logger, std::move(c.modDir), std::move(fm)));
    }

    // Sort descending by priority (index 0 = highest priority)
    std::sort(packs.begin(), packs.end(), [](const auto& a, const auto& b) { return a->priority() > b->priority(); });

    return packs;
}
