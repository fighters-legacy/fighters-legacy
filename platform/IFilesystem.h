// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Distinguishes the read-only asset root (install directory) from the writable
// user-data root (per-user app data). Required for Linux Flatpak and macOS
// sandbox compliance where these directories are separate OS-managed paths.
enum class PathDomain : uint8_t {
    Assets,   // read-only; content packs, mods, missions
    UserData  // writable; saves, user config, logs
};

// Seek origin for IFilesystem::seek, mirroring SEEK_SET / SEEK_CUR / SEEK_END.
enum class SeekOrigin : uint8_t {
    Begin,
    Current,
    End
};

// IMPORTANT: This is a synchronous, blocking interface. readFile will not return
// until the OS delivers the data. It is correct for startup asset loading, mod
// discovery, and config reads. Do NOT call it on the main thread for per-frame
// terrain streaming; a separate async I/O design is planned for Phase 2.
class IFilesystem {
public:
    virtual ~IFilesystem() = default;

    struct Entry {
        std::string name;
        bool isDirectory;
    };

    // Returns an opaque file handle (>= 0) on success, or -1 on failure.
    // write=true creates the file if it does not exist and truncates if it does.
    virtual int openFile(PathDomain domain, const char* path, bool write) = 0;
    virtual void closeFile(int handle) = 0;

    virtual std::size_t readFile(int handle, void* buffer, std::size_t size) = 0;
    virtual std::size_t writeFile(int handle, const void* data, std::size_t size) = 0;

    // Random access; returns false if the seek is out of range.
    virtual bool seek(int handle, std::size_t offset, SeekOrigin origin) = 0;

    // Returns the total size of the open file in bytes.
    virtual std::size_t getFileSize(int handle) const = 0;

    virtual bool fileExists(PathDomain domain, const char* path) const = 0;

    // Creates the directory (and any missing parents). Returns true if the
    // directory exists on return, whether or not it was just created.
    virtual bool createDirectory(PathDomain domain, const char* path) = 0;

    // Atomically renames (moves) a file within the same domain. Use this for safe
    // save-file writes: write to a temp path, then rename to the final path.
    virtual bool renameFile(PathDomain domain, const char* from, const char* to) = 0;

    // Returns all entries directly inside the given directory. Each Entry reports
    // whether it is a file or a subdirectory, which ModLoader uses to distinguish
    // mod folders from loose files.
    virtual std::vector<Entry> scanDirectory(PathDomain domain,
                                             const char* path) const = 0;
};
