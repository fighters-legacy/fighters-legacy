// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ILogger.h"
#include <cstdio>

// Minimal ILogger implementation that writes to stdout.
// For use in headless binaries (fl-server) that have no file-based logger.
class StdoutLogger : public ILogger {
  public:
    void log(LogLevel level, const char* /*file*/, int /*line*/, const char* message) override {
        if (level < m_minLevel)
            return;
        const char* tag = level == LogLevel::Trace   ? "TRACE"
                          : level == LogLevel::Debug ? "DEBUG"
                          : level == LogLevel::Info  ? "INFO "
                          : level == LogLevel::Warn  ? "WARN "
                                                     : "ERROR";
        std::printf("[%s] %s\n", tag, message);
        std::fflush(stdout);
    }
    void setMinLevel(LogLevel minLevel) override {
        m_minLevel = minLevel;
    }

  private:
    LogLevel m_minLevel{LogLevel::Info};
    void flush() override {
        std::fflush(stdout);
    }
};
