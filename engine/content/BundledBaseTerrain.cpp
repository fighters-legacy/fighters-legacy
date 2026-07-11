// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/BundledBaseTerrain.h"

#include "IFilesystem.h"
#include "ILogger.h"
#include "content/FolderContentPack.h"

#include <limits>

namespace fl {

std::unique_ptr<IContentPack> loadBundledBaseTerrain(IFilesystem& fs, ILogger& logger, const std::string& baseDir,
                                                     const std::string& terrainId) {
    // Sentinel: the +X face (0) level-0 root height tile. A bundled base always ships the six
    // level-0 face roots, so its presence means a base is installed. Absent => return nullptr so
    // the procedural-only launch keeps hasPacks()==false (no per-tile pack probe).
    const std::string sentinel = baseDir + "/terrain/" + terrainId + "/f0/l0/tile_0_0.png";
    if (!fs.fileExists(PathDomain::Assets, sentinel.c_str()))
        return nullptr;

    FolderContentPack::Manifest m;
    m.name = "Bundled base terrain";
    m.id = "builtin:base-terrain";
    m.version = "1.0.0";
    m.engineApi = "1.0";
    m.priority = std::numeric_limits<int>::min(); // lowest — every user pack overrides it
    m.trustLevel = TrustLevel::Maintainer;        // shipped with the engine
    m.nativePlugin = false;

    logger.log(LogLevel::Info, __FILE__, __LINE__,
               (std::string("terrain: mounting bundled base '") + baseDir + "' for '" + terrainId + "'").c_str());
    return std::make_unique<FolderContentPack>(fs, logger, baseDir, std::move(m));
}

} // namespace fl
