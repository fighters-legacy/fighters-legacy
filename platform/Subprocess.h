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

    // Run a program to completion and return its exit code, or -1 if it could not be spawned or did
    // not exit normally (#1265).
    //
    // Separate from spawn() above because it answers a different question: no pipes, no lifetime, no
    // object — just "run this and tell me how it went". tools/tex-compress had its own
    // _spawnvp/posix_spawnp pair for exactly this, which meant two cross-platform process spawners
    // in one codebase.
    //
    // ⚠ ARGV, NOT A COMMAND LINE. argv[0] is the program and the rest are passed through untouched:
    // no shell is involved on any platform, so nothing in an argument can be interpreted as a
    // command. That is a security property tex-compress documented and depends on (it passes
    // user-supplied file paths), which is why this does NOT reuse spawn()'s Windows path — that one
    // joins the arguments into a single command-line string with its own quoting rules.
    [[nodiscard]] static int runAndWait(const std::vector<std::string>& argv);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fl
