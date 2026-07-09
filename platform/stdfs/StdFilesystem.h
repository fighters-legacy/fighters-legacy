// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IFilesystem.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_map>

namespace fl {

// std::filesystem / std::fstream backed IFilesystem. No windowing/library
// dependencies — used by both the headless server and the GUI client. Callers
// resolve the assets/user-data roots (the server uses the CWD; the client uses
// SDL path helpers) and pass them in; this backend knows nothing about SDL.
class StdFilesystem : public IFilesystem {
  public:
    StdFilesystem(std::filesystem::path assetsRoot, std::filesystem::path userDataRoot);
    ~StdFilesystem() override = default;

    int openFile(PathDomain domain, const char* path, bool write) override;
    void closeFile(int handle) override;

    std::size_t readFile(int handle, void* buffer, std::size_t size) override;
    std::size_t writeFile(int handle, const void* data, std::size_t size) override;

    bool seek(int handle, std::size_t offset, SeekOrigin origin) override;
    std::size_t getFileSize(int handle) const override;

    bool fileExists(PathDomain domain, const char* path) const override;
    bool createDirectory(PathDomain domain, const char* path) override;
    bool renameFile(PathDomain domain, const char* from, const char* to) override;
    std::vector<Entry> scanDirectory(PathDomain domain, const char* path) const override;

  private:
    const std::filesystem::path& root(PathDomain domain) const;

    std::filesystem::path m_assetsRoot;
    std::filesystem::path m_userDataRoot;

    int m_nextHandle = 1;
    std::unordered_map<int, std::unique_ptr<std::fstream>> m_handles;
};

} // namespace fl
