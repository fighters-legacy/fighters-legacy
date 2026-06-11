// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class ILogger;
class CommandRegistry;

// Thread-safe output ring + command dispatch sink.
// GameConsole inherits from this; fl-server uses it directly for admin I/O.
// print() is safe to call from the sim thread (via enqueueSimCallback).
class CommandShell {
  public:
    explicit CommandShell(ILogger& logger, CommandRegistry& registry);
    virtual ~CommandShell() = default;

    // Append a line to the ring. Thread-safe (callable from sim-thread callbacks).
    void print(std::string line);

    // Dispatch a command: echo "> line", call registry.dispatch(), push result lines
    // to the ring, log to logger. Returns the raw dispatch result string.
    virtual std::string execute(std::string_view line);

    // Returns a copy of the ring, oldest first, most-recent last. Thread-safe.
    [[nodiscard]] std::vector<std::string> outputLines() const;

    // High-water-mark drain API for RconServer async-confirmation polling.
    // mark() returns a snapshot of the write counter; drainSince(mark) returns all
    // entries written after that mark, oldest-first. Both are thread-safe.
    [[nodiscard]] int mark() const;
    [[nodiscard]] std::vector<std::string> drainSince(int mark) const;

  protected:
    // Ring fields are protected so GameConsole::buildHud() can read them directly.
    // Any reader must hold m_ringMutex while accessing them.
    static constexpr int kMaxOutputLines = 64;
    std::array<std::string, kMaxOutputLines> m_outputRing;
    int m_outputHead{0};
    int m_outputCount{0};
    int m_totalWritten{0}; // monotonically incrementing write counter for drainSince()
    mutable std::mutex m_ringMutex;

    void pushOutput(std::string line); // must be called while holding m_ringMutex
    ILogger& m_logger;
    CommandRegistry& m_registry;
};
