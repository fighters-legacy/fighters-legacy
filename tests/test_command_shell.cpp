// SPDX-License-Identifier: GPL-3.0-or-later
#include "ILogger.h"
#include "console/CommandRegistry.h"
#include "console/CommandShell.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

struct NullShellLogger : public ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

TEST_CASE("CommandShell outputLines empty on construction", "[shell]") {
    NullShellLogger logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);
    REQUIRE(shell.outputLines().empty());
}

TEST_CASE("CommandShell print appends in order", "[shell]") {
    NullShellLogger logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);

    shell.print("first");
    shell.print("second");
    shell.print("third");

    auto lines = shell.outputLines();
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "first");
    CHECK(lines[1] == "second");
    CHECK(lines[2] == "third");
}

TEST_CASE("CommandShell execute pushes echo and result to ring", "[shell]") {
    NullShellLogger logger;
    CommandRegistry reg;
    reg.registerCommand("cmd", "test", [](std::span<std::string_view>) { return std::string("ok"); });
    CommandShell shell(logger, reg);

    shell.execute("cmd");

    auto lines = shell.outputLines();
    REQUIRE(lines.size() >= 2);
    bool foundEcho = false, foundResult = false;
    for (const auto& l : lines) {
        if (l.find("> cmd") != std::string::npos)
            foundEcho = true;
        if (l.find("ok") != std::string::npos)
            foundResult = true;
    }
    CHECK(foundEcho);
    CHECK(foundResult);
}

TEST_CASE("CommandShell ring wraps after kMaxOutputLines", "[shell]") {
    NullShellLogger logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);

    for (int i = 1; i <= 65; ++i)
        shell.print("line" + std::to_string(i));

    auto lines = shell.outputLines();
    REQUIRE(lines.size() == 64);
    CHECK(lines.front() == "line2");
    CHECK(lines.back() == "line65");
}

TEST_CASE("CommandShell print thread safety", "[shell]") {
    NullShellLogger logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);

    std::thread writer([&shell] {
        for (int i = 0; i < 1000; ++i)
            shell.print("msg" + std::to_string(i));
    });

    // Read concurrently — must not crash or data-race under TSAN
    for (int i = 0; i < 100; ++i)
        (void)shell.outputLines();

    writer.join();

    auto lines = shell.outputLines();
    REQUIRE(lines.size() <= 64);
}

// ---------------------------------------------------------------------------
// mark() / drainSince() tests
// ---------------------------------------------------------------------------

TEST_CASE("CommandShell mark returns 0 on empty shell", "[shell][drain]") {
    NullShellLogger logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);
    CHECK(shell.mark() == 0);
}

TEST_CASE("CommandShell mark advances with each print", "[shell][drain]") {
    NullShellLogger logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);

    shell.print("a");
    shell.print("b");
    shell.print("c");
    CHECK(shell.mark() == 3);
}

TEST_CASE("CommandShell drainSince returns only new lines", "[shell][drain]") {
    NullShellLogger logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);

    shell.print("before");
    int m = shell.mark();
    shell.print("after1");
    shell.print("after2");

    auto drained = shell.drainSince(m);
    REQUIRE(drained.size() == 2);
    CHECK(drained[0] == "after1");
    CHECK(drained[1] == "after2");
}

TEST_CASE("CommandShell drainSince returns empty when nothing new", "[shell][drain]") {
    NullShellLogger logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);

    shell.print("line");
    int m = shell.mark();

    CHECK(shell.drainSince(m).empty());
}

TEST_CASE("CommandShell drainSince successive calls with updated mark", "[shell][drain]") {
    NullShellLogger logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);

    shell.print("line1");
    int m1 = shell.mark();
    shell.print("line2");

    auto first = shell.drainSince(m1);
    REQUIRE(first.size() == 1);
    CHECK(first[0] == "line2");

    int m2 = shell.mark();
    CHECK(shell.drainSince(m2).empty());
}

TEST_CASE("CommandShell drainSince clamps at kMaxOutputLines on overflow", "[shell][drain]") {
    NullShellLogger logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);

    // Fill ring
    for (int i = 0; i < 64; ++i)
        shell.print("pre" + std::to_string(i));
    int m = shell.mark();

    // Write more than kMaxOutputLines (64) lines after mark
    for (int i = 0; i < 70; ++i)
        shell.print("post" + std::to_string(i));

    auto drained = shell.drainSince(m);
    // Ring can only hold 64 entries; oldest overwritten entries are silently dropped
    REQUIRE(drained.size() == 64);
    CHECK(drained.back() == "post69");
}

TEST_CASE("CommandShell drainSince after ring wrap returns only post-mark entries", "[shell][drain]") {
    NullShellLogger logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);

    // Overflow ring by 1
    for (int i = 0; i < 65; ++i)
        shell.print("pre" + std::to_string(i));
    int m = shell.mark();

    shell.print("new1");
    shell.print("new2");

    auto drained = shell.drainSince(m);
    REQUIRE(drained.size() == 2);
    CHECK(drained[0] == "new1");
    CHECK(drained[1] == "new2");
}

TEST_CASE("CommandShell drainSince is thread-safe under concurrent print", "[shell][drain]") {
    NullShellLogger logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);

    std::atomic<int> drainCount{0};
    int m = shell.mark();

    std::thread writer([&shell] {
        for (int i = 0; i < 1000; ++i)
            shell.print("msg" + std::to_string(i));
    });

    // Concurrent drainSince — must not crash or data-race under TSAN
    for (int i = 0; i < 200; ++i) {
        auto lines = shell.drainSince(m);
        drainCount.fetch_add(static_cast<int>(lines.size()), std::memory_order_relaxed);
    }

    writer.join();
    // Just verify no crash and count is non-negative
    CHECK(drainCount.load() >= 0);
}
