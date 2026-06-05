// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "content/IContentPack.h"
#include "content/TrustLevel.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

class IContentPackEventHandler;
class IFilesystem;
class ILogger;

class ModLoader {
  public:
    struct Manifest {
        std::string name;
        std::string id;
        std::string version;
        std::string engineApi;
        int priority = 0;
        std::vector<std::string> depends;
        TrustLevel trustLevel = TrustLevel::Unsigned;
        bool nativePlugin = false;
    };

    struct LoadError {
        std::string path;   // mod directory that failed
        std::string modId;  // empty if manifest was unparseable
        std::string reason; // human-readable
    };

    // assetsAbsoluteRoot: absolute filesystem path to the Assets domain root.
    // When non-empty, native compiled plugins (.dll/.so/.dylib) are loaded from
    // this root. When empty (e.g. in tests), plugin detection still fires events
    // but the dlopen/LoadLibrary step is skipped.
    ModLoader(IFilesystem& fs, ILogger& logger, std::string assetsAbsoluteRoot = {});

    // Scans PathDomain::Assets/"mods", parses manifests, returns content packs
    // sorted by priority descending (index 0 = highest). Packs that fail to parse,
    // have an incompatible engine-api, or fail manifest sanitization are skipped.
    // Missing dependencies log a Warn but do not prevent loading.
    // handler: optional receiver for security events (untrusted packs, native plugins).
    // Call getLoadErrors() after load() to retrieve any failures.
    std::vector<std::unique_ptr<IContentPack>> load(IContentPackEventHandler* handler = nullptr);

    const std::vector<LoadError>& getLoadErrors() const {
        return m_loadErrors;
    }

  private:
    static constexpr const char* kModsDir = "mods";
    static constexpr const char* kEngineApiMajor = "1";

    std::optional<Manifest> parseManifest(const char* path);
    bool validateEngineApi(const std::string& engineApi, const std::string& modId);

    IFilesystem& m_fs;
    ILogger& m_logger;
    std::string m_assetsAbsoluteRoot;
    std::vector<LoadError> m_loadErrors;
};
