// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

enum class LogLevel : uint8_t { Trace, Debug, Info, Warn, Error };

// Platform-specific logging backend (OutputDebugString on Windows, os_log on
// macOS, stderr/journald on Linux). Engine code should call this through a thin
// macro wrapper defined in engine/ that injects __FILE__ and __LINE__ and
// formats the message into a stack buffer (no heap allocation at call sites).
// Threading: log() must be thread-safe; it may be called simultaneously from the
// main thread, audio thread, and network thread. All other methods are main-thread only.
class ILogger {
  public:
    virtual ~ILogger() = default;

    // file and message are const char* to avoid string allocation on any call path.
    virtual void log(LogLevel level, const char* file, int line, const char* message) = 0;

    // Suppresses messages below minLevel (e.g. set Info in release builds).
    virtual void setMinLevel(LogLevel minLevel) = 0;

    // Flushes any buffered output to the underlying sink. Call from a crash handler
    // to ensure the last log lines survive before the process exits.
    virtual void flush() = 0;
};
