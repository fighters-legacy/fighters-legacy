// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IFilesystem.h"
#include <string>
#include <vector>

// Polling-based filesystem change notifications for hot-reload in sandbox/editor
// mode. Not available in campaign mode. Threading: all methods main-thread only.
class IFilesystemWatcher {
  public:
    virtual ~IFilesystemWatcher() = default;

    enum class EventType : uint8_t { Created, Modified, Deleted, Renamed };

    struct Event {
        std::string path; // path relative to the domain root
        EventType type;
    };

    // Register a directory for monitoring.
    // recursive=true watches all subdirectories too (required for mods with nested assets).
    // Returns false if the directory does not exist or cannot be watched.
    virtual bool watch(PathDomain domain, const char* path, bool recursive = false) = 0;
    virtual void unwatch(PathDomain domain, const char* path) = 0;

    // Returns and clears all pending events since the last call. Non-blocking.
    virtual std::vector<Event> pollEvents() = 0;
};
