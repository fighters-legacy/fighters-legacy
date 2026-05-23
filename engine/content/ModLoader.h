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

    ModLoader(IFilesystem& fs, ILogger& logger);

    // Scans PathDomain::Assets/"mods", parses manifests, returns content packs
    // sorted by priority descending (index 0 = highest). Packs that fail to parse
    // or have an incompatible engine-api major version are skipped. Missing
    // dependencies log a Warn but do not prevent loading.
    std::vector<std::unique_ptr<IContentPack>> load();

  private:
    static constexpr const char* kModsDir = "mods";
    static constexpr const char* kEngineApiMajor = "1";

    std::optional<Manifest> parseManifest(const char* path);
    bool validateEngineApi(const std::string& engineApi, const std::string& modId);

    IFilesystem& m_fs;
    ILogger& m_logger;
};
