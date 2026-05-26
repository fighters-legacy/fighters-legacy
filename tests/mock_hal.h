// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IFilesystem.h"
#include "ILogger.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

struct MockLogger : public ILogger {
    struct Entry {
        LogLevel level;
        std::string message;
    };
    std::vector<Entry> entries;

    void log(LogLevel level, const char*, int, const char* message) override {
        entries.push_back({level, message});
    }
    void setMinLevel(LogLevel) override {}
    void flush() override {}

    bool hasMessage(LogLevel level, const std::string& substr) const {
        for (auto& e : entries)
            if (e.level == level && e.message.find(substr) != std::string::npos)
                return true;
        return false;
    }
};

struct MockFilesystem : public IFilesystem {
    std::map<std::string, std::vector<uint8_t>> files;
    std::map<std::string, std::vector<Entry>> dirs;

    bool createDirectoryResult = true;
    bool failWriteOpen = false;
    bool renameResult = true;

    struct RenameCall {
        std::string from;
        std::string to;
    };
    std::vector<RenameCall> renameCalls;

    void addFile(const std::string& path, const std::string& content) {
        files[path] = std::vector<uint8_t>(content.begin(), content.end());
    }

    int openFile(PathDomain, const char* path, bool write) override {
        if (write) {
            if (failWriteOpen)
                return -1;
            files[path] = {};
            writeHandles[nextHandle] = path;
            return nextHandle++;
        }
        auto it = files.find(path);
        if (it == files.end())
            return -1;
        readHandles[nextHandle] = path;
        return nextHandle++;
    }

    void closeFile(int handle) override {
        readHandles.erase(handle);
        writeHandles.erase(handle);
    }

    std::size_t readFile(int handle, void* buffer, std::size_t size) override {
        auto hit = readHandles.find(handle);
        if (hit == readHandles.end())
            return 0;
        auto& data = files[hit->second];
        std::size_t n = std::min(size, data.size());
        std::memcpy(buffer, data.data(), n);
        return n;
    }

    std::size_t writeFile(int handle, const void* data, std::size_t size) override {
        auto hit = writeHandles.find(handle);
        if (hit == writeHandles.end())
            return 0;
        auto& buf = files[hit->second];
        const auto* bytes = static_cast<const uint8_t*>(data);
        buf.insert(buf.end(), bytes, bytes + size);
        return size;
    }

    bool seek(int, std::size_t, SeekOrigin) override {
        return false;
    }

    std::size_t getFileSize(int handle) const override {
        auto hit = readHandles.find(handle);
        if (hit == readHandles.end())
            return 0;
        auto fit = files.find(hit->second);
        return (fit != files.end()) ? fit->second.size() : 0;
    }

    bool fileExists(PathDomain, const char* path) const override {
        return files.find(path) != files.end();
    }

    bool createDirectory(PathDomain, const char*) override {
        return createDirectoryResult;
    }

    bool renameFile(PathDomain, const char* from, const char* to) override {
        renameCalls.push_back({from, to});
        if (renameResult && files.count(from)) {
            files[to] = std::move(files[from]);
            files.erase(from);
        }
        return renameResult;
    }

    std::vector<Entry> scanDirectory(PathDomain, const char* path) const override {
        auto it = dirs.find(path);
        if (it == dirs.end())
            return {};
        return it->second;
    }

  private:
    int nextHandle = 1;
    std::map<int, std::string> readHandles;
    std::map<int, std::string> writeHandles;
};
