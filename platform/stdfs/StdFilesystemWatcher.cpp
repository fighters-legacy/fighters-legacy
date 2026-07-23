// SPDX-License-Identifier: GPL-3.0-or-later
#include "StdFilesystemWatcher.h"

#include <algorithm>
#include <cstdio>
#include <system_error>

namespace fs = std::filesystem;

namespace fl {

StdFilesystemWatcher::StdFilesystemWatcher(fs::path assetsRoot, fs::path userDataRoot, uint32_t pollIntervalMs,
                                           uint32_t maxFilesPerWatch, ILogger* logger)
    : m_assetsRoot(std::move(assetsRoot)), m_userDataRoot(std::move(userDataRoot)), m_pollIntervalMs(pollIntervalMs),
      m_maxFilesPerWatch(maxFilesPerWatch), m_logger(logger) {}

const fs::path& StdFilesystemWatcher::domainRoot(PathDomain domain) const {
    return domain == PathDomain::Assets ? m_assetsRoot : m_userDataRoot;
}

namespace {
// Enumerate the files under `absDir` (recursively when asked), keyed by their path RELATIVE to
// `domainRoot` with forward slashes. error_code overloads throughout — a permission error or a file
// that vanishes mid-scan is skipped, never thrown. Returns false if the count exceeds `cap`.
bool enumerate(const fs::path& absDir, const fs::path& domainRoot, bool recursive, uint32_t cap,
               std::unordered_map<std::string, StdFilesystemWatcher::FileSig>* out) {
    std::error_code ec;
    auto record = [&](const fs::directory_entry& e) -> bool {
        if (!e.is_regular_file(ec))
            return true;
        fs::path rel = fs::relative(e.path(), domainRoot, ec);
        if (ec)
            return true;
        std::string key = rel.generic_string();
        StdFilesystemWatcher::FileSig sig;
        sig.mtime = e.last_write_time(ec);
        sig.size = e.file_size(ec);
        if (out)
            (*out)[std::move(key)] = sig;
        return out == nullptr || out->size() <= cap;
    };
    if (recursive) {
        fs::recursive_directory_iterator it(absDir, fs::directory_options::skip_permission_denied, ec), end;
        if (ec)
            return false;
        for (; it != end; it.increment(ec)) {
            if (ec)
                break;
            if (!record(*it))
                return false;
        }
    } else {
        fs::directory_iterator it(absDir, fs::directory_options::skip_permission_denied, ec), end;
        if (ec)
            return false;
        for (; it != end; it.increment(ec)) {
            if (ec)
                break;
            if (!record(*it))
                return false;
        }
    }
    return true;
}
} // namespace

bool StdFilesystemWatcher::watch(PathDomain domain, const char* path, bool recursive) {
    const std::string rel = path ? path : "";
    const fs::path absDir = rel.empty() ? domainRoot(domain) : (domainRoot(domain) / rel);
    std::error_code ec;
    if (!fs::is_directory(absDir, ec))
        return false; // the directory does not exist / cannot be watched

    WatchState w;
    w.domain = domain;
    w.relPath = rel;
    w.recursive = recursive;
    w.absDir = absDir;
    if (!enumerate(absDir, domainRoot(domain), recursive, m_maxFilesPerWatch, &w.known)) {
        if (m_logger) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "watch dropped (over %u files): %s", m_maxFilesPerWatch,
                          absDir.string().c_str());
            m_logger->log(LogLevel::Warn, __FILE__, __LINE__, buf);
        }
        return false;
    }
    m_watches.push_back(std::move(w));
    return true;
}

void StdFilesystemWatcher::unwatch(PathDomain domain, const char* path) {
    const std::string rel = path ? path : "";
    m_watches.erase(std::remove_if(m_watches.begin(), m_watches.end(),
                                   [&](const WatchState& w) { return w.domain == domain && w.relPath == rel; }),
                    m_watches.end());
}

void StdFilesystemWatcher::scanWatch(WatchState& w, std::vector<Event>& out) {
    std::unordered_map<std::string, FileSig> current;
    // On failure (dir removed mid-run), treat as empty -> everything Deleted.
    enumerate(w.absDir, domainRoot(w.domain), w.recursive, m_maxFilesPerWatch, &current);

    // Additions / modifications, with the two-scan settle.
    for (auto& [rel, sig] : current) {
        auto knownIt = w.known.find(rel);
        const bool isNew = knownIt == w.known.end();
        if (!isNew && knownIt->second == sig) {
            w.pending.erase(rel); // stable and unchanged
            continue;
        }
        // Candidate create/modify: require two identical consecutive scans before emitting.
        auto pendIt = w.pending.find(rel);
        if (pendIt != w.pending.end() && pendIt->second == sig) {
            out.push_back({rel, isNew ? EventType::Created : EventType::Modified});
            w.known[rel] = sig;
            w.pending.erase(pendIt);
        } else {
            w.pending[rel] = sig; // first sighting (or a still-changing signature)
        }
    }

    // Deletions fire immediately.
    for (auto it = w.known.begin(); it != w.known.end();) {
        if (current.find(it->first) == current.end()) {
            out.push_back({it->first, EventType::Deleted});
            w.pending.erase(it->first);
            it = w.known.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<IFilesystemWatcher::Event> StdFilesystemWatcher::pollEvents() {
    // Throttle: rescan at most every m_pollIntervalMs (0 = always, for tests). The first call always
    // scans.
    const auto now = std::chrono::steady_clock::now();
    if (m_scannedOnce && m_pollIntervalMs > 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastScan).count();
        if (elapsed < static_cast<long long>(m_pollIntervalMs))
            return {};
    }
    m_lastScan = now;
    m_scannedOnce = true;

    std::vector<Event> events;
    for (auto& w : m_watches)
        scanWatch(w, events);
    return events;
}

} // namespace fl
