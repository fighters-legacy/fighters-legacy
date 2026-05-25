// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "IFilesystem.h"
#include "ILogger.h"
#include "config/UserConfig.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Mock types (mirrors test_firstrun.cpp)
// ---------------------------------------------------------------------------

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
    bool createDirectoryResult = true;
    bool renameResult = true;

    void addFile(const std::string& path, const std::string& content) {
        files[path] = std::vector<uint8_t>(content.begin(), content.end());
    }

    int openFile(PathDomain, const char* path, bool write) override {
        if (write) {
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
        if (renameResult && files.count(from)) {
            files[to] = std::move(files[from]);
            files.erase(from);
        }
        return renameResult;
    }
    std::vector<Entry> scanDirectory(PathDomain, const char*) const override {
        return {};
    }

  private:
    int nextHandle = 1;
    std::map<int, std::string> readHandles;
    std::map<int, std::string> writeHandles;
};

// ---------------------------------------------------------------------------
// parseLogLevel tests
// ---------------------------------------------------------------------------

TEST_CASE("parseLogLevel: known strings map correctly", "[userconfig]") {
    CHECK(parseLogLevel("debug") == LogLevel::Debug);
    CHECK(parseLogLevel("info") == LogLevel::Info);
    CHECK(parseLogLevel("warn") == LogLevel::Warn);
    CHECK(parseLogLevel("error") == LogLevel::Error);
}

TEST_CASE("parseLogLevel: unknown string falls back to Info", "[userconfig]") {
    CHECK(parseLogLevel("verbose") == LogLevel::Info);
    CHECK(parseLogLevel("UNKNOWN") == LogLevel::Info);
    CHECK(parseLogLevel("") == LogLevel::Info);
    CHECK(parseLogLevel(nullptr) == LogLevel::Info);
}

// ---------------------------------------------------------------------------
// UserConfig log level round-trip tests
// ---------------------------------------------------------------------------

TEST_CASE("UserConfig: logLevel default is Info when [engine] section absent", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[first_run]\ncompleted = false\n");
    UserConfig config(fs, logger);
    config.load();
    CHECK(config.logLevel() == LogLevel::Info);
}

TEST_CASE("UserConfig: setLogLevel + save + reload round-trip for Debug", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    config.setLogLevel(LogLevel::Debug);
    config.save();

    // Reload into a fresh config
    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.logLevel() == LogLevel::Debug);
}

TEST_CASE("UserConfig: setLogLevel + save + reload round-trip for Warn", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    config.setLogLevel(LogLevel::Warn);
    config.save();

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.logLevel() == LogLevel::Warn);
}

TEST_CASE("UserConfig: setLogLevel + save + reload round-trip for Error", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    config.setLogLevel(LogLevel::Error);
    config.save();

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.logLevel() == LogLevel::Error);
}

TEST_CASE("UserConfig: setLogLevel + save + reload round-trip for Info", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    config.setLogLevel(LogLevel::Info);
    config.save();

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.logLevel() == LogLevel::Info);
}

TEST_CASE("UserConfig: unknown log_level string in TOML falls back to Info and emits Warn", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[engine]\nlog_level = \"verbose\"\n");
    UserConfig config(fs, logger);
    config.load();
    CHECK(config.logLevel() == LogLevel::Info);
    CHECK(logger.hasMessage(LogLevel::Warn, "verbose"));
}
