// SPDX-License-Identifier: GPL-3.0-or-later
#include "StdFilesystem.h"

#include <ios>
#include <system_error>

namespace fs = std::filesystem;

namespace fl {

StdFilesystem::StdFilesystem(fs::path assetsRoot, fs::path userDataRoot)
    : m_assetsRoot(std::move(assetsRoot)), m_userDataRoot(std::move(userDataRoot)) {}

const fs::path& StdFilesystem::root(PathDomain domain) const {
    return domain == PathDomain::Assets ? m_assetsRoot : m_userDataRoot;
}

int StdFilesystem::openFile(PathDomain domain, const char* path, bool write) {
    fs::path full = root(domain) / path;
    // Open from the fs::path object (not a narrow string) so Windows uses the
    // wide native path internally — keeps UTF-8 paths working cross-platform.
    // write=true creates+truncates ("wb"); read opens existing binary ("rb").
    std::ios::openmode mode =
        write ? (std::ios::out | std::ios::binary | std::ios::trunc) : (std::ios::in | std::ios::binary);
    auto stream = std::make_unique<std::fstream>(full, mode);
    if (!stream->is_open())
        return -1;
    int id = m_nextHandle++;
    m_handles[id] = std::move(stream);
    return id;
}

void StdFilesystem::closeFile(int handle) {
    m_handles.erase(handle);
}

std::size_t StdFilesystem::readFile(int handle, void* buffer, std::size_t size) {
    auto it = m_handles.find(handle);
    if (it == m_handles.end())
        return 0;
    std::fstream& s = *it->second;
    s.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
    std::size_t got = static_cast<std::size_t>(s.gcount());
    // A short read hits EOF/fail; clear so the handle stays usable for seeks.
    if (!s)
        s.clear();
    return got;
}

std::size_t StdFilesystem::writeFile(int handle, const void* data, std::size_t size) {
    auto it = m_handles.find(handle);
    if (it == m_handles.end())
        return 0;
    std::fstream& s = *it->second;
    s.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return s.good() ? size : 0;
}

bool StdFilesystem::seek(int handle, std::size_t offset, SeekOrigin origin) {
    auto it = m_handles.find(handle);
    if (it == m_handles.end())
        return false;
    std::ios::seekdir dir;
    switch (origin) {
    case SeekOrigin::Begin:
        dir = std::ios::beg;
        break;
    case SeekOrigin::Current:
        dir = std::ios::cur;
        break;
    case SeekOrigin::End:
        dir = std::ios::end;
        break;
    default:
        dir = std::ios::beg;
        break;
    }
    std::fstream& s = *it->second;
    s.clear(); // drop any prior EOF/fail so the seek can proceed
    s.seekg(static_cast<std::streamoff>(offset), dir);
    s.seekp(static_cast<std::streamoff>(offset), dir);
    return !s.fail();
}

std::size_t StdFilesystem::getFileSize(int handle) const {
    auto it = m_handles.find(handle);
    if (it == m_handles.end())
        return 0;
    std::fstream& s = *it->second;
    std::streampos cur = s.tellg();
    s.seekg(0, std::ios::end);
    std::streampos end = s.tellg();
    s.seekg(cur, std::ios::beg);
    return end >= 0 ? static_cast<std::size_t>(end) : 0;
}

bool StdFilesystem::fileExists(PathDomain domain, const char* path) const {
    std::error_code ec;
    return fs::exists(root(domain) / path, ec);
}

bool StdFilesystem::createDirectory(PathDomain domain, const char* path) {
    try {
        fs::create_directories(root(domain) / path);
        return true;
    } catch (...) {
        return false;
    }
}

bool StdFilesystem::renameFile(PathDomain domain, const char* from, const char* to) {
    try {
        fs::rename(root(domain) / from, root(domain) / to);
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<IFilesystem::Entry> StdFilesystem::scanDirectory(PathDomain domain, const char* path) const {
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

} // namespace fl
