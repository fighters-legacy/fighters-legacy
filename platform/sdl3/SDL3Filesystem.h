// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IFilesystem.h"

#include <filesystem>
#include <string>
#include <unordered_map>

struct SDL_IOStream;

namespace fl {

class SDL3Filesystem : public IFilesystem {
  public:
    SDL3Filesystem(std::filesystem::path assetsRoot, std::filesystem::path userDataRoot);
    ~SDL3Filesystem() override;

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
    std::unordered_map<int, SDL_IOStream*> m_handles;
};

} // namespace fl
