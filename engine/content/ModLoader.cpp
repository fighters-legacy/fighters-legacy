// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/ModLoader.h"

#include "IFilesystem.h"
#include "ILogger.h"
#include "content/FolderContentPack.h"
#include "content/IContentPackEventHandler.h"

#include <toml++/toml.hpp>

#if defined(_WIN32)
#include <windows.h> // LoadLibraryA, GetProcAddress, FreeLibrary
#else
#include <dlfcn.h> // dlopen, dlsym, dlclose
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace fl {

// ---------------------------------------------------------------------------
// Manifest sanitization helpers
// ---------------------------------------------------------------------------

static bool isWindowsReservedName(std::string_view component) {
    // Strip extension before comparing (e.g. "NUL.toml" → "NUL")
    auto dot = component.rfind('.');
    if (dot != std::string_view::npos)
        component = component.substr(0, dot);

    if (component.empty())
        return false;

    static const char* kReserved[] = {
        "CON",  "NUL",  "PRN",  "AUX",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    for (const char* r : kReserved) {
        if (component.size() != std::strlen(r))
            continue;
        bool match = true;
        for (size_t i = 0; i < component.size(); ++i) {
            if (std::toupper(static_cast<unsigned char>(component[i])) != static_cast<unsigned char>(r[i])) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

// Returns true if `field` is safe to use as an id or name manifest field.
// Rejects: length > 128, null bytes, path separators, drive letters, Windows reserved names.
static bool isValidIdentifier(std::string_view field) {
    if (field.size() > 128)
        return false;
    // Null bytes
    if (field.find('\0') != std::string_view::npos)
        return false;
    // Path separators
    if (field.find('/') != std::string_view::npos)
        return false;
    if (field.find('\\') != std::string_view::npos)
        return false;
    // Drive-letter prefix (e.g. "C:")
    if (field.size() >= 2 && std::isalpha(static_cast<unsigned char>(field[0])) && field[1] == ':')
        return false;
    // Windows reserved device names
    if (isWindowsReservedName(field))
        return false;
    return true;
}

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

    // Sanitize id and name: must be safe path-component identifiers
    if (!isValidIdentifier(manifest.id)) {
        m_logger.log(LogLevel::Error, __FILE__, __LINE__,
                     (std::string("manifest '") + path + "': invalid id field '" + manifest.id + "'").c_str());
        return std::nullopt;
    }
    if (!isValidIdentifier(manifest.name)) {
        m_logger.log(LogLevel::Error, __FILE__, __LINE__,
                     (std::string("manifest '") + path + "': invalid name field '" + manifest.name + "'").c_str());
        return std::nullopt;
    }

    // Optional depends array
    if (auto deps = mod["depends"].as_array()) {
        for (auto& dep : *deps) {
            if (auto s = dep.value<std::string>())
                manifest.depends.push_back(std::move(*s));
        }
    }

    // Optional [mod.trust] section — parsed but GPG not verified until Phase 6
    if (auto trust = tbl["mod"]["trust"]) {
        if (auto sig = trust["signature"].value<std::string>(); sig && !sig->empty()) {
            m_logger.log(LogLevel::Info, __FILE__, __LINE__,
                         ("mod '" + manifest.id + "': signature present but not verified in this build").c_str());
        }
        if (auto signedBy = trust["signed-by"].value<std::string>()) {
            if (*signedBy == "community") {
                manifest.trustLevel = TrustLevel::Community;
            } else if (*signedBy == "maintainer") {
                manifest.trustLevel = TrustLevel::Maintainer;
            } else {
                m_logger.log(
                    LogLevel::Warn, __FILE__, __LINE__,
                    ("mod '" + manifest.id + "': unknown signed-by value '" + *signedBy + "' — defaulting to Unsigned")
                        .c_str());
            }
        }
    }

    return manifest;
}

// ---------------------------------------------------------------------------
// Native plugin loading helper
// ---------------------------------------------------------------------------

static IContentPack* loadNativePlugin(const std::string& absolutePath, ILogger& logger) {
#if defined(_WIN32)
    HMODULE handle = LoadLibraryA(absolutePath.c_str());
    if (!handle) {
        logger.log(LogLevel::Error, __FILE__, __LINE__,
                   ("failed to load plugin '" + absolutePath + "' via LoadLibrary").c_str());
        return nullptr;
    }
    using FactoryFn = IContentPack* (*)();
    auto* factory = reinterpret_cast<FactoryFn>(GetProcAddress(handle, IContentPack::kFactorySymbol));
    if (!factory) {
        logger.log(LogLevel::Error, __FILE__, __LINE__,
                   ("plugin '" + absolutePath + "': symbol '" + IContentPack::kFactorySymbol + "' not found").c_str());
        FreeLibrary(handle);
        return nullptr;
    }
#else
    void* handle = dlopen(absolutePath.c_str(), RTLD_LOCAL | RTLD_NOW);
    if (!handle) {
        const char* err = dlerror();
        logger.log(
            LogLevel::Error, __FILE__, __LINE__,
            (std::string("failed to load plugin '") + absolutePath + "': " + (err ? err : "unknown error")).c_str());
        return nullptr;
    }
    using FactoryFn = IContentPack* (*)();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* factory = reinterpret_cast<FactoryFn>(dlsym(handle, IContentPack::kFactorySymbol));
    if (!factory) {
        const char* err = dlerror();
        logger.log(LogLevel::Error, __FILE__, __LINE__,
                   (std::string("plugin '") + absolutePath + "': symbol '" + IContentPack::kFactorySymbol +
                    "' not found: " + (err ? err : "unknown error"))
                       .c_str());
        dlclose(handle);
        return nullptr;
    }
#endif
    // Plugin handle intentionally not stored — plugins are loaded for process lifetime.
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
            if (handler) {
                handler->onNativeCodePackLoaded(*pluginPack);
                if (c.manifest.trustLevel == TrustLevel::Unsigned)
                    handler->onUntrustedPackLoaded(*pluginPack);
            }
            packs.push_back(std::move(pluginPack));
        } else {
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
