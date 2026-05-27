// SPDX-License-Identifier: GPL-3.0-or-later
#include "SDL3Filesystem.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

SDL3Filesystem::SDL3Filesystem(fs::path assetsRoot, fs::path userDataRoot)
    : m_assetsRoot(std::move(assetsRoot)), m_userDataRoot(std::move(userDataRoot)) {}

SDL3Filesystem::~SDL3Filesystem() {
    for (auto& [id, stream] : m_handles)
        SDL_CloseIO(stream);
}

const fs::path& SDL3Filesystem::root(PathDomain domain) const {
    return domain == PathDomain::Assets ? m_assetsRoot : m_userDataRoot;
}

int SDL3Filesystem::openFile(PathDomain domain, const char* path, bool write) {
    fs::path full = root(domain) / path;
    const char* mode = write ? "wb" : "rb";
    SDL_IOStream* stream = SDL_IOFromFile(full.string().c_str(), mode);
    if (!stream)
        return -1;
    int id = m_nextHandle++;
    m_handles[id] = stream;
    return id;
}

void SDL3Filesystem::closeFile(int handle) {
    auto it = m_handles.find(handle);
    if (it == m_handles.end())
        return;
    SDL_CloseIO(it->second);
    m_handles.erase(it);
}

std::size_t SDL3Filesystem::readFile(int handle, void* buffer, std::size_t size) {
    auto it = m_handles.find(handle);
    if (it == m_handles.end())
        return 0;
    return SDL_ReadIO(it->second, buffer, size);
}

std::size_t SDL3Filesystem::writeFile(int handle, const void* data, std::size_t size) {
    auto it = m_handles.find(handle);
    if (it == m_handles.end())
        return 0;
    return SDL_WriteIO(it->second, data, size);
}

bool SDL3Filesystem::seek(int handle, std::size_t offset, SeekOrigin origin) {
    auto it = m_handles.find(handle);
    if (it == m_handles.end())
        return false;
    SDL_IOWhence whence;
    switch (origin) {
    case SeekOrigin::Begin:
        whence = SDL_IO_SEEK_SET;
        break;
    case SeekOrigin::Current:
        whence = SDL_IO_SEEK_CUR;
        break;
    case SeekOrigin::End:
        whence = SDL_IO_SEEK_END;
        break;
    default:
        whence = SDL_IO_SEEK_SET;
        break;
    }
    return SDL_SeekIO(it->second, static_cast<Sint64>(offset), whence) >= 0;
}

std::size_t SDL3Filesystem::getFileSize(int handle) const {
    auto it = m_handles.find(handle);
    if (it == m_handles.end())
        return 0;
    Sint64 sz = SDL_GetIOSize(it->second);
    return sz >= 0 ? static_cast<std::size_t>(sz) : 0;
}

bool SDL3Filesystem::fileExists(PathDomain domain, const char* path) const {
    std::error_code ec;
    return fs::exists(root(domain) / path, ec);
}

bool SDL3Filesystem::createDirectory(PathDomain domain, const char* path) {
    try {
        fs::create_directories(root(domain) / path);
        return true;
    } catch (...) {
        return false;
    }
}

bool SDL3Filesystem::renameFile(PathDomain domain, const char* from, const char* to) {
    try {
        fs::rename(root(domain) / from, root(domain) / to);
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<IFilesystem::Entry> SDL3Filesystem::scanDirectory(PathDomain domain, const char* path) const {
    fs::path dir = root(domain) / path;
    std::error_code ec;
    if (!fs::exists(dir, ec))
        return {};
    try {
        std::vector<Entry> entries;
        for (const auto& de : fs::directory_iterator(dir))
            entries.push_back({de.path().filename().string(), de.is_directory()});
        return entries;
    } catch (...) {
        return {};
    }
}
