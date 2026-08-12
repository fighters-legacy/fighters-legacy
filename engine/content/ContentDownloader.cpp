// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/ContentDownloader.h"

namespace fl {

void ContentDownloader::start(std::vector<ContentManifestEntry> manifest, DoneFn onDone) {
    m_manifest = std::move(manifest);
    m_onDone = std::move(onDone);
    m_results.clear();
    m_current = 0;
    m_active = true;
    if (m_manifest.empty()) {
        finish(true);
        return;
    }
    beginEntry(0);
}

void ContentDownloader::beginEntry(std::size_t i) {
    const ContentManifestEntry& e = m_manifest[i];
    m_current = i;
    m_hasher.reset();
    m_received = 0;
    m_total = 0;
    m_writeFailed = false;
    m_tmpPath = e.destPath + ".part";

    m_writeHandle = m_fs.openFile(PathDomain::UserData, m_tmpPath.c_str(), /*write=*/true);
    if (m_writeHandle < 0) {
        m_log.log(LogLevel::Error, __FILE__, __LINE__, "content download: cannot open temp file for write");
        finishEntry(false, "cannot open temp file");
        return;
    }

    HttpRequestOptions opts;
    opts.url = e.url;
    opts.userAgent = "fighters-legacy-content/1";
    m_activeId = m_http.get(opts, this);
    if (m_activeId == 0) {
        m_fs.closeFile(m_writeHandle);
        m_writeHandle = -1;
        finishEntry(false, "request could not be enqueued");
    }
}

bool ContentDownloader::onHttpData(HttpRequestId id, const void* data, std::size_t len) {
    if (id != m_activeId || m_writeHandle < 0)
        return false;
    if (m_fs.writeFile(m_writeHandle, data, len) != len) {
        m_writeFailed = true;
        return false; // abort the transfer; onHttpComplete(Cancelled) follows
    }
    m_hasher.update(data, len);
    m_received += len;
    return true;
}

void ContentDownloader::onHttpProgress(HttpRequestId id, uint64_t received, uint64_t total) {
    if (id != m_activeId)
        return;
    m_received = received;
    m_total = total;
}

void ContentDownloader::onHttpComplete(HttpRequestId id, HttpStatus status, long httpCode, const char* errorMsg) {
    if (id != m_activeId)
        return;
    if (m_writeHandle >= 0) {
        m_fs.closeFile(m_writeHandle);
        m_writeHandle = -1;
    }

    if (m_writeFailed) {
        truncatePartial();
        finishEntry(false, "write failed");
        return;
    }
    if (status != HttpStatus::Success) {
        truncatePartial();
        finishEntry(false, status == HttpStatus::Cancelled ? "cancelled" : (errorMsg ? errorMsg : "transfer error"));
        return;
    }
    if (httpCode != 200) {
        truncatePartial();
        finishEntry(false, "HTTP " + std::to_string(httpCode));
        return;
    }

    const std::string got = sha256Hex(m_hasher.finalize());
    const std::string& want = m_manifest[m_current].sha256Hex;
    if (got != want) {
        truncatePartial(); // a mismatched transfer must never become a real file
        m_log.log(LogLevel::Error, __FILE__, __LINE__, "content download: SHA-256 mismatch, rejecting");
        finishEntry(false, "sha256 mismatch");
        return;
    }

    const std::string& dest = m_manifest[m_current].destPath;
    if (!m_fs.renameFile(PathDomain::UserData, m_tmpPath.c_str(), dest.c_str())) {
        finishEntry(false, "rename into place failed");
        return;
    }
    finishEntry(true, {});
}

void ContentDownloader::truncatePartial() {
    // No delete in IFilesystem; reopening for write truncates to zero so no partial bytes linger.
    const int h = m_fs.openFile(PathDomain::UserData, m_tmpPath.c_str(), /*write=*/true);
    if (h >= 0)
        m_fs.closeFile(h);
}

void ContentDownloader::finishEntry(bool ok, const std::string& error) {
    m_results.push_back({m_manifest[m_current].destPath, ok, error});
    m_activeId = 0;
    if (m_current + 1 < m_manifest.size()) {
        beginEntry(m_current + 1);
    } else {
        bool allOk = true;
        for (const auto& r : m_results)
            allOk = allOk && r.ok;
        finish(allOk);
    }
}

void ContentDownloader::finish(bool allOk) {
    m_active = false;
    // No slot to restore, and nothing to stomp (#1083). This used to be
    // `m_http.setEventHandler(nullptr)`, which did not restore whatever was there -- and the client
    // already owned that slot for the server browser's lobby list, so the first completed download
    // silently ended lobby callbacks for the rest of the process.
    if (m_onDone)
        m_onDone(allOk);
}

} // namespace fl
