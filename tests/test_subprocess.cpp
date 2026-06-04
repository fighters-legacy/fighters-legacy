// SPDX-License-Identifier: GPL-3.0-or-later
#include "ILogger.h"
#include "Subprocess.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

struct SilentLogger : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// Platform-portable "echo hello" command.
#if defined(_WIN32)
// Pass the stem only — Subprocess::spawn appends ".exe" on Windows.
static const char* kEchoBin = "cmd";
static const std::vector<std::string> kEchoArgs{"/c", "echo hello"};
#else
static const char* kEchoBin = "sh";
static const std::vector<std::string> kEchoArgs{"-c", "echo hello"};
#endif

TEST_CASE("Subprocess spawn nonexistent binary returns invalid", "[subprocess]") {
    SilentLogger log;
    Subprocess sub = Subprocess::spawn("/definitely/does/not/exist/binary", {}, true, false, log);
    REQUIRE_FALSE(sub.valid());
}

TEST_CASE("Subprocess spawn echo command, read stdout, process exits", "[subprocess]") {
    SilentLogger log;
    Subprocess sub = Subprocess::spawn(kEchoBin, kEchoArgs,
                                       /*captureStdout=*/true, /*captureStdin=*/false, log);
    REQUIRE(sub.valid());

    // Read the output line (up to 2 s timeout).
    auto line = sub.readStdoutLine(2000);
    REQUIRE(line.has_value());
    // The output should contain "hello" (Windows cmd may include trailing space).
    REQUIRE(line->find("hello") != std::string::npos);

    // Wait briefly for process to exit naturally.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE_FALSE(sub.isRunning());
}

TEST_CASE("Subprocess stop on already-exited process does not crash", "[subprocess]") {
    SilentLogger log;
    Subprocess sub = Subprocess::spawn(kEchoBin, kEchoArgs, false, false, log);
    // Let it exit naturally.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    // Calling stop() on an already-exited process must not crash.
    REQUIRE_NOTHROW(sub.stop());
}
