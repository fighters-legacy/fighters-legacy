// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IFilesystem.h"
#include "IHttpClient.h"
#include "ILogger.h"
#include "crypto/Sha256.h"

#include <functional>
#include <string>
#include <vector>

// Verified content download (#490). Streams a small manifest of files over IHttpClient into
// PathDomain::UserData, hashing each incrementally and only renaming a `.part` into place when the
// SHA-256 matches the manifest — so a corrupt or truncated transfer never lands as a real file. The
// engine stays HAL-clean: ContentDownloader takes IHttpClient + IFilesystem by injection. This is the
// substrate a first-run "download the base content pack" flow drives; the community mod index (#108)
// consumes the same path.
//
// Entries download SEQUENTIALLY (one write handle + one hasher at a time). Drive it by calling
// IHttpClient::service() each frame; completion is reported via the start() callback + results().

namespace fl {

struct ContentManifestEntry {
    std::string url;
    std::string sha256Hex; // expected digest, lower-case hex (64 chars)
    std::string destPath;  // destination, relative to PathDomain::UserData
    uint64_t sizeBytes{0}; // optional/informational (0 = unknown)
};

struct ContentDownloadResult {
    std::string destPath;
    bool ok{false};
    std::string error; // empty on success
};

class ContentDownloader : public IHttpClientHandler {
  public:
    using DoneFn = std::function<void(bool allOk)>;

    ContentDownloader(IHttpClient& http, IFilesystem& fs, ILogger& log) : m_http(http), m_fs(fs), m_log(log) {}

    // Begin downloading `manifest` sequentially. onDone fires once, after the last entry, with true
    // iff every entry verified. No-op (onDone(true)) for an empty manifest. Registers itself as the
    // IHttpClient handler for the duration.
    void start(std::vector<ContentManifestEntry> manifest, DoneFn onDone = {});

    [[nodiscard]] bool inProgress() const noexcept {
        return m_active;
    }
    [[nodiscard]] const std::vector<ContentDownloadResult>& results() const noexcept {
        return m_results;
    }
    // received/total bytes of the current entry (total 0 = server sent no length).
    [[nodiscard]] uint64_t currentReceived() const noexcept {
        return m_received;
    }
    [[nodiscard]] uint64_t currentTotal() const noexcept {
        return m_total;
    }

    // IHttpClientHandler
    bool onHttpData(HttpRequestId id, const void* data, std::size_t len) override;
    void onHttpProgress(HttpRequestId id, uint64_t received, uint64_t total) override;
    void onHttpComplete(HttpRequestId id, HttpStatus status, long httpCode, const char* errorMsg) override;

  private:
    void beginEntry(std::size_t i);
    void finishEntry(bool ok, const std::string& error);
    void finish(bool allOk);
    void truncatePartial(); // zero the `.part` so a rejected transfer leaves no partial bytes

    IHttpClient& m_http;
    IFilesystem& m_fs;
    ILogger& m_log;

    std::vector<ContentManifestEntry> m_manifest;
    std::vector<ContentDownloadResult> m_results;
    DoneFn m_onDone;

    std::size_t m_current{0};
    HttpRequestId m_activeId{0};
    int m_writeHandle{-1};
    Sha256 m_hasher;
    std::string m_tmpPath;
    uint64_t m_received{0};
    uint64_t m_total{0};
    bool m_active{false};
    bool m_writeFailed{false};
};

} // namespace fl
