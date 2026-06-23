// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ILogger.h"
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace fl {

class FileLogger : public ILogger {
  public:
    ~FileLogger() override {
        close();
    }

    // Opens (or creates) a new session log in logDir. Rotates old logs to keep at
    // most maxRetained files. Returns false if the directory cannot be created or
    // the file cannot be opened; log() calls are silently no-ops until open() succeeds.
    bool open(const std::string& logDir, int maxRetained = 10);
    void close();
    bool isOpen() const;

    // ILogger
    void log(LogLevel level, const char* file, int line, const char* message) override;
    void setMinLevel(LogLevel minLevel) override;
    void flush() override;

    struct RingEntry {
        LogLevel level;
        char file[256]; // 256 bytes; Windows absolute __FILE__ paths can be long
        int line;
        char message[512];
    };

    // Copies up to maxLines most-recent entries into out. Safe to call from a signal
    // handler: reads ring indices atomically, no mutex. Returns count written.
    int copyLastLines(RingEntry* out, int maxLines) const;

    const std::string& currentLogPath() const {
        return m_logPath;
    }

  private:
    static constexpr int kRingCap = 256;

    mutable std::mutex m_mutex;
    FILE* m_file{nullptr};
    LogLevel m_minLevel{LogLevel::Info};
    std::string m_logPath;

    RingEntry m_ring[kRingCap];
    std::atomic<int> m_ringHead{0};  // next write slot (mod kRingCap); atomic for signal-handler safety
    std::atomic<int> m_ringCount{0}; // total entries written, capped at kRingCap; atomic for signal-handler safety

    void enforceRetention(const std::string& logDir, int maxRetained);
    static std::string makeFilename(const std::string& logDir);
    static const char* levelTag(LogLevel l);
};

} // namespace fl
