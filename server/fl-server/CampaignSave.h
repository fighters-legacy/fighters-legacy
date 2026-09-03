// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Where a campaign's saved war comes from, and where it goes (#534, D25).
//
// D25 absorbs the campaign `.flsave` into the store as an opaque blob column. That is a change to
// something an operator already has on disk, so it is not just a swap: an existing war has to
// survive the upgrade, a server with persistence disabled has to keep working exactly as before,
// and a store that has taken over must never silently rewind to a stale file left beside it.
//
// It lives here, apart from ServerRuntime, for the reason openConfiguredStore does: the resolution
// order IS the behaviour, and inside a ServerRuntime::Impl method nothing could reach it. Running a
// whole campaign to test a precedence rule is not a test anyone writes twice.

#include <string>

namespace fl {

class ILogger;

namespace persist {
class IPersistence;
}

// The two places a campaign's state can live. `store` may be null, or may be the null store
// (persistence disabled) — both mean "the file is the record".
struct CampaignSaveIo {
    persist::IPersistence* store{nullptr};
    std::string blobKey;  // "campaign/<sanitized name>"
    std::string filePath; // "cache/campaign_<sanitized name>.flsave"
};

// What was loaded, and where it came from — the caller logs the difference, and a test asserts it.
struct CampaignSaveLoad {
    std::string blob;         // empty = no saved state anywhere
    bool fromStore{false};    // true = the store had it
    bool importedFile{false}; // true = a pre-store file was found and copied into the store
};

// Store first, file second.
//
// The order is the whole point. Reading the file first would let a stale `.flsave` — left behind by
// the import, because deleting someone's war record buys nothing — overwrite a war the store has
// been advancing for weeks.
[[nodiscard]] CampaignSaveLoad loadCampaignSave(const CampaignSaveIo& io, ILogger* log);

// Store when it is open, file otherwise. Turning persistence off must not cost an operator their
// campaign, so the file path stays a first-class destination rather than a dead branch.
//
// ⚠ Call on the MAIN thread. The campaign's end hook fires on the sim thread, which may not touch
// the store (IPersistence.h) — fl-server posts this through GameLoop::enqueueMainCallback.
void saveCampaignSave(const CampaignSaveIo& io, const std::string& blob, ILogger* log);

} // namespace fl
