// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IFilesystemWatcher.h"
#include "ILogger.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

// A polling std::filesystem watcher — the one production IFilesystemWatcher backend (#152). No
// OS-native APIs (inotify / ReadDirectoryChangesW): the interface is documented as polling, and a
// recursive_directory_iterator rescan is portable and adequate for the small watched dirs (asset
// subdirs; terrain tile trees are deliberately not watched — see AssetManager::enableHotReload).
//
// Two-scan settle: a Created/Modified signature is emitted only after it is seen IDENTICAL across two
// consecutive scans, which defeats an editor's partial write deterministically (no time-based
// debounce, so tests need no sleeps). Deleted fires immediately. Renamed is NEVER emitted — a
// rename-replace save shows up as Deleted + Created, which the AssetManager reverse-map handles.
//
// Threading: main thread only, like IFilesystem.
class StdFilesystemWatcher : public IFilesystemWatcher {
  public:
    // Roots mirror StdFilesystem's (Assets -> assetsRoot, UserData -> userDataRoot). pollIntervalMs
    // throttles the rescan (0 = rescan on every pollEvents(), used by tests). maxFilesPerWatch caps a
    // single watch's file count (a watch over the cap is dropped with a Warn). logger may be null.
    StdFilesystemWatcher(std::filesystem::path assetsRoot, std::filesystem::path userDataRoot,
                         uint32_t pollIntervalMs = 250, uint32_t maxFilesPerWatch = 20000, ILogger* logger = nullptr);

    bool watch(PathDomain domain, const char* path, bool recursive = false) override;
    void unwatch(PathDomain domain, const char* path) override;
    std::vector<Event> pollEvents() override;

    // A file's change signature (public so the .cpp's enumerate helper can name it).
    struct FileSig {
        std::filesystem::file_time_type mtime{};
        uintmax_t size{0};
        bool operator==(const FileSig& o) const noexcept {
            return mtime == o.mtime && size == o.size;
        }
        bool operator!=(const FileSig& o) const noexcept {
            return !(*this == o);
        }
    };

  private:
    struct WatchState {
        PathDomain domain;
        std::string relPath; // relative to the domain root (forward-slash), "" = the domain root
        bool recursive{false};
        std::filesystem::path absDir;                     // resolved absolute directory
        std::unordered_map<std::string, FileSig> known;   // emitted signatures (relative to domain root)
        std::unordered_map<std::string, FileSig> pending; // candidate seen once, awaiting a second scan
    };

    const std::filesystem::path& domainRoot(PathDomain domain) const;
    // Rescan one watch and append its events. Applies the two-scan settle.
    void scanWatch(WatchState& w, std::vector<Event>& out);

    std::filesystem::path m_assetsRoot;
    std::filesystem::path m_userDataRoot;
    uint32_t m_pollIntervalMs;
    uint32_t m_maxFilesPerWatch;
    ILogger* m_logger;
    std::vector<WatchState> m_watches;
    std::chrono::steady_clock::time_point m_lastScan{};
    bool m_scannedOnce{false};
};

} // namespace fl
