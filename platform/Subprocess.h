// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fl {

class ILogger;

// Cross-platform child process manager with stdin/stdout pipe support.
// All platform-specific code (CreateProcess vs posix_spawn, WriteFile vs write, etc.)
// is confined to Subprocess.cpp. Callers are #ifdef-free.
//
// Follows the FileLogger model: concrete class, not an interface.
class Subprocess {
  public:
    Subprocess() = default;
    ~Subprocess();
    Subprocess(Subprocess&&) noexcept;
    Subprocess& operator=(Subprocess&&) noexcept;

    // Spawn binary (path without extension — .exe appended on Windows internally).
    // captureStdout/captureStdin wire pipes for readStdoutLine/writeStdin.
    // Returns an invalid Subprocess (valid()==false) on launch failure.
    static Subprocess spawn(const std::string& binaryPath, const std::vector<std::string>& args, bool captureStdout,
                            bool captureStdin, ILogger& log);

    bool valid() const;
    bool isRunning() const; // false once the process has exited

    // Write line to child stdin (appends '\n').
    void writeStdin(std::string_view line);

    // Read one line from child stdout with a timeout. Returns nullopt on timeout or EOF.
    std::optional<std::string> readStdoutLine(int timeoutMs);

    // Graceful: writeStdin("quit") + wait 2 s; forceful kill on timeout.
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fl
