// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <share.h> // _SH_DENYNO for _fsopen
#endif

#include "FileLogger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const char* FileLogger::levelTag(LogLevel l) {
    switch (l) {
    case LogLevel::Trace:
        return "[TRACE]";
    case LogLevel::Debug:
        return "[DEBUG]";
    case LogLevel::Info:
        return "[INFO ]";
    case LogLevel::Warn:
        return "[WARN ]";
    case LogLevel::Error:
        return "[ERROR]";
    }
    return "[INFO ]";
}

static std::string currentTimestamp() {
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64]; // 64 bytes: actual output is 23 chars; larger buf silences GCC format-truncation
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));
    return buf;
}

// ---------------------------------------------------------------------------
// makeFilename — logs/engine_YYYY-MM-DD_HH-MM-SS.log
// ---------------------------------------------------------------------------

std::string FileLogger::makeFilename(const std::string& logDir) {
    using clock = std::chrono::system_clock;
    std::time_t t = clock::to_time_t(clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[128]; // 128 bytes: actual output is ~38 chars; larger buf silences GCC format-truncation
    std::snprintf(buf, sizeof(buf), "engine_%04d-%02d-%02d_%02d-%02d-%02d.log", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return (fs::path(logDir) / buf).string();
}

// ---------------------------------------------------------------------------
// enforceRetention — keep at most maxRetained engine_*.log files
// ---------------------------------------------------------------------------

void FileLogger::enforceRetention(const std::string& logDir, int maxRetained) {
    std::vector<fs::path> logs;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(logDir, ec)) {
        auto name = entry.path().filename().string();
        if (name.rfind("engine_", 0) == 0 && entry.path().extension() == ".log")
            logs.push_back(entry.path());
    }
    if (ec || static_cast<int>(logs.size()) < maxRetained)
        return;

    std::sort(logs.begin(), logs.end());                            // lexicographic = chronological (timestamp suffix)
    int toDelete = static_cast<int>(logs.size()) - maxRetained + 1; // +1 to make room for new file
    for (int i = 0; i < toDelete; ++i)
        fs::remove(logs[static_cast<std::size_t>(i)], ec);
}

// ---------------------------------------------------------------------------
// open / close / isOpen
// ---------------------------------------------------------------------------

bool FileLogger::open(const std::string& logDir, int maxRetained) {
    std::error_code ec;
    fs::create_directories(logDir, ec);
    if (ec)
        return false;

    enforceRetention(logDir, maxRetained);

    m_logPath = makeFilename(logDir);
#if defined(_WIN32)
    m_file = _fsopen(m_logPath.c_str(), "w", _SH_DENYNO);
#else
    m_file = std::fopen(m_logPath.c_str(), "w");
#endif
    return m_file != nullptr;
}

void FileLogger::close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file) {
        std::fflush(m_file);
        std::fclose(m_file);
        m_file = nullptr;
    }
}

bool FileLogger::isOpen() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_file != nullptr;
}

// ---------------------------------------------------------------------------
// ILogger
// ---------------------------------------------------------------------------

void FileLogger::log(LogLevel level, const char* file, int line, const char* message) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (level < m_minLevel)
        return;

    // Write to ring buffer (atomic indices updated after entry is written)
    int slot = m_ringHead.load(std::memory_order_relaxed) % kRingCap;
    RingEntry& entry = m_ring[slot];
    entry.level = level;
    // Truncate file from the left if it exceeds the field (preserve filename)
    if (file) {
        std::size_t len = std::strlen(file);
        if (len >= sizeof(entry.file)) {
            std::size_t skip = len - (sizeof(entry.file) - 1);
            std::memcpy(entry.file, file + skip, sizeof(entry.file) - 1);
        } else {
            std::memcpy(entry.file, file, len + 1);
        }
        entry.file[sizeof(entry.file) - 1] = '\0';
    } else {
        entry.file[0] = '\0';
    }
    entry.line = line;
    if (message) {
        std::size_t msgLen = std::min(std::strlen(message), sizeof(entry.message) - 1);
        std::memcpy(entry.message, message, msgLen);
        entry.message[msgLen] = '\0';
    } else {
        entry.message[0] = '\0';
    }
    m_ringHead.fetch_add(1, std::memory_order_release);
    int prev = m_ringCount.load(std::memory_order_relaxed);
    if (prev < kRingCap)
        m_ringCount.store(prev + 1, std::memory_order_release);

    // Write to file
    if (m_file) {
        std::fprintf(m_file, "%s %s  %s:%d  %s\n", levelTag(level), currentTimestamp().c_str(), file ? file : "", line,
                     message ? message : "");
    }
}

void FileLogger::setMinLevel(LogLevel minLevel) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_minLevel = minLevel;
}

void FileLogger::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file)
        std::fflush(m_file);
}

// ---------------------------------------------------------------------------
// copyLastLines — signal-handler safe (no mutex)
// ---------------------------------------------------------------------------

int FileLogger::copyLastLines(RingEntry* out, int maxLines) const {
    if (!out || maxLines <= 0)
        return 0;

    int count = m_ringCount.load(std::memory_order_acquire);
    int head = m_ringHead.load(std::memory_order_acquire);

    int available = std::min(count, kRingCap);
    int want = std::min(available, maxLines);
    if (want <= 0)
        return 0;

    // Oldest entry is at (head - available) mod kRingCap
    int start = ((head - available) % kRingCap + kRingCap) % kRingCap;
    // Skip entries we don't want (grab the most recent `want`)
    int skip = available - want;
    int readStart = (start + skip) % kRingCap;

    for (int i = 0; i < want; ++i) {
        out[i] = m_ring[(readStart + i) % kRingCap];
    }
    return want;
}
