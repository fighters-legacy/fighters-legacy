// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/ModLoader.h"

#include "content/ModManifest.h"

#include "IFilesystem.h"
#include "ILogger.h"
#include "content/FolderContentPack.h"
#include "content/IContentPackEventHandler.h"
#include "dynlib/DynLib.h" // the shared dlopen/LoadLibrary this file used to keep to itself (#163)
#include "util/FsRead.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <unordered_set>

namespace fl {

// ---------------------------------------------------------------------------
// Manifest sanitization helpers
// ---------------------------------------------------------------------------

ModLoader::ModLoader(IFilesystem& fs, ILogger& logger, std::string assetsAbsoluteRoot)
    : m_fs(fs), m_logger(logger), m_assetsAbsoluteRoot(std::move(assetsAbsoluteRoot)) {}

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
    const auto read = readFileToString(m_fs, PathDomain::Assets, path);
    if (!read)
        return std::nullopt; // missing manifest.toml — silently skip at Debug level
    const std::string& content = *read;

    // Delegate the schema to the shared parser (#651), so ModLoader and validate-mod cannot drift on
    // the required-field / identifier rules. ModLoader keeps its log-and-skip contract by logging the
    // accumulated errors here and returning nullopt on any of them.
    ModManifestParseResult res = parseModManifest(content);
    for (const std::string& w : res.warnings)
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__, (std::string("manifest '") + path + "': " + w).c_str());
    for (const std::string& e : res.errors)
        m_logger.log(LogLevel::Error, __FILE__, __LINE__, (std::string("manifest '") + path + "': " + e).c_str());
    if (!res.ok)
        return std::nullopt;

    // A present signature is noted (GPG verification is Phase 6). ModLoader-level Info, not a parser
    // finding, so it stays out of the validator's error/warning surface.
    if (res.manifest.hasSignature)
        m_logger.log(LogLevel::Info, __FILE__, __LINE__,
                     ("mod '" + res.manifest.id + "': signature present but not verified in this build").c_str());

    Manifest manifest;
    manifest.name = std::move(res.manifest.name);
    manifest.id = std::move(res.manifest.id);
    manifest.version = std::move(res.manifest.version);
    manifest.engineApi = std::move(res.manifest.engineApi);
    manifest.namespaceId = std::move(res.manifest.namespaceId);
    manifest.priority = res.manifest.priority;
    manifest.depends = std::move(res.manifest.depends);
    manifest.trustLevel = res.manifest.trustLevel;
    return manifest;
}

// ---------------------------------------------------------------------------
// Native plugin loading helper
// ---------------------------------------------------------------------------

static IContentPack* loadNativePlugin(const std::string& absolutePath, ILogger& logger) {
    // The platform half moved to engine/dynlib (#163) when the AI provider seam became the second
    // thing in the tree that loads a factory symbol out of a shared library. The handle is still
    // deliberately never closed: plugins load for process lifetime.
    void* sym = loadLibrarySymbol(absolutePath, IContentPack::kFactorySymbol, logger);
    if (!sym)
        return nullptr;
    using FactoryFn = IContentPack* (*)();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto factory = reinterpret_cast<FactoryFn>(sym);
    return factory();
}

// Returns the platform-specific native plugin filename for a mod id.
// e.g. id="example" → "libexample.so" on Linux, "example.dll" on Windows.
static std::string nativePluginFilename(const std::string& id) {
#if defined(_WIN32)
    return id + ".dll";
#elif defined(__APPLE__)
    return "lib" + id + ".dylib";
#else
    return "lib" + id + ".so";
#endif
}

std::vector<std::unique_ptr<IContentPack>> ModLoader::load(IContentPackEventHandler* handler) {
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
            m_loadErrors.push_back({modDir, "", "manifest parse or sanitization failed (see log)"});
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

    // Build packs: detect native plugins, propagate trust, attempt plugin loading
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.reserve(candidates.size());
    for (auto& c : candidates) {
        // Detect native plugin file alongside the manifest
        std::string pluginFilename = nativePluginFilename(c.manifest.id);
        std::string pluginRelPath = c.modDir + "/" + pluginFilename;
        if (m_fs.fileExists(PathDomain::Assets, pluginRelPath.c_str()))
            c.manifest.nativePlugin = true;

        FolderContentPack::Manifest fm;
        fm.name = c.manifest.name;
        fm.id = c.manifest.id;
        fm.namespaceId = c.manifest.namespaceId;
        fm.version = c.manifest.version;
        fm.engineApi = c.manifest.engineApi;
        fm.priority = c.manifest.priority;
        fm.trustLevel = c.manifest.trustLevel;
        fm.nativePlugin = c.manifest.nativePlugin;

        if (c.manifest.nativePlugin && !m_assetsAbsoluteRoot.empty()) {
            // Attempt to load the compiled plugin with a full absolute path to
            // prevent DLL planting. On failure, skip this candidate entirely.
            std::string absPluginPath = m_assetsAbsoluteRoot + "/" + pluginRelPath;
            IContentPack* raw = loadNativePlugin(absPluginPath, m_logger);
            if (!raw) {
                m_loadErrors.push_back({c.modDir, c.manifest.id, "native plugin failed to load"});
                continue;
            }
            std::unique_ptr<IContentPack> pluginPack(raw);
            m_logger.log(LogLevel::Info, __FILE__, __LINE__,
                         ("loaded native plugin '" + c.manifest.id + "' from " + pluginRelPath).c_str());
            if (handler) {
                handler->onNativeCodePackLoaded(*pluginPack);
                if (c.manifest.trustLevel == TrustLevel::Unsigned)
                    handler->onUntrustedPackLoaded(*pluginPack);
            }
            packs.push_back(std::move(pluginPack));
        } else {
            if (c.manifest.nativePlugin) {
                m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                             ("mod '" + c.manifest.id +
                              "': native plugin detected but plugin loading is disabled (no assets root) — "
                              "loading as a folder pack")
                                 .c_str());
            }
            auto folderPack = std::make_unique<FolderContentPack>(m_fs, m_logger, std::move(c.modDir), std::move(fm));
            if (handler) {
                if (c.manifest.nativePlugin)
                    handler->onNativeCodePackLoaded(*folderPack);
                if (c.manifest.trustLevel == TrustLevel::Unsigned)
                    handler->onUntrustedPackLoaded(*folderPack);
            }
            packs.push_back(std::move(folderPack));
        }
    }

    // Sort descending by priority (index 0 = highest priority)
    std::sort(packs.begin(), packs.end(), [](const auto& a, const auto& b) { return a->priority() > b->priority(); });

    return packs;
}

} // namespace fl
