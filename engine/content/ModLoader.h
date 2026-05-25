// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "content/IContentPack.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
    };

    struct LoadError {
        std::string path;   // mod directory that failed
        std::string modId;  // empty if manifest was unparseable
        std::string reason; // human-readable
    };

    ModLoader(IFilesystem& fs, ILogger& logger);

    // Scans PathDomain::Assets/"mods", parses manifests, returns content packs
    // sorted by priority descending (index 0 = highest). Packs that fail to parse
    // or have an incompatible engine-api major version are skipped. Missing
    // dependencies log a Warn but do not prevent loading.
    // Call getLoadErrors() after load() to retrieve any failures.
    std::vector<std::unique_ptr<IContentPack>> load();

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
    std::vector<LoadError> m_loadErrors;
};
