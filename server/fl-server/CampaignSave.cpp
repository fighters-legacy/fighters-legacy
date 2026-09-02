// SPDX-License-Identifier: GPL-3.0-or-later
#include "CampaignSave.h"

#include <IPersistence.h>

#include <ILogger.h>
#include <config/ConfigFile.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace fl {
namespace {

std::vector<std::byte> toBytes(const std::string& s) {
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    return std::vector<std::byte>(p, p + s.size());
}

std::string fromBytes(const std::vector<std::byte>& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

bool storeUsable(const CampaignSaveIo& io) {
    // The null store answers every read with nullopt and swallows every write, so treating it as
    // usable would silently drop campaigns on a server that merely turned persistence off.
    return io.store != nullptr && io.store->health().open;
}

} // namespace

CampaignSaveLoad loadCampaignSave(const CampaignSaveIo& io, ILogger* log) {
    CampaignSaveLoad out;

    if (io.store != nullptr && !io.blobKey.empty()) {
        if (auto bytes = io.store->blobs().get(io.blobKey)) {
            out.blob = fromBytes(*bytes);
            out.fromStore = true;
            return out;
        }
    }

    if (io.filePath.empty())
        return out;
    std::ifstream in{io.filePath, std::ios::binary};
    if (!in)
        return out;
    out.blob.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (out.blob.empty())
        return out;

    // A war that predates the store: copy it in, once. The FILE IS LEFT WHERE IT IS -- it is an
    // operator's campaign record, deleting it buys nothing, and a downgrade to an older fl-server
    // has to still find it.
    if (storeUsable(io) && !io.blobKey.empty()) {
        io.store->blobs().put(io.blobKey, toBytes(out.blob));
        out.importedFile = true;
        if (log) {
            char buf[320];
            std::snprintf(buf, sizeof(buf), "campaign: imported %s into the store as '%s' (the file is left in place)",
                          io.filePath.c_str(), io.blobKey.c_str());
            log->log(LogLevel::Info, __FILE__, __LINE__, buf);
        }
    }
    return out;
}

void saveCampaignSave(const CampaignSaveIo& io, const std::string& blob, ILogger* log) {
    if (storeUsable(io) && !io.blobKey.empty()) {
        io.store->blobs().put(io.blobKey, toBytes(blob));
        return;
    }
    if (!io.filePath.empty())
        writeConfigFile(io.filePath, blob, *log);
}

} // namespace fl
