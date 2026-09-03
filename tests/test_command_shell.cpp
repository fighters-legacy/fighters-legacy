// SPDX-License-Identifier: GPL-3.0-or-later
#include "ILogger.h"
#include "console/CommandRegistry.h"
#include "console/CommandShell.h"
#include "mock_log.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace fl;

TEST_CASE("CommandShell outputLines empty on construction", "[shell]") {
    NullLogger logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);
    REQUIRE(shell.outputLines().empty());
}

TEST_CASE("CommandShell print appends in order", "[shell]") {
    NullLogger logger;
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
    NullLogger logger;
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
    NullLogger logger;
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
    NullLogger logger;
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
    NullLogger logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);
    CHECK(shell.mark() == 0);
}

TEST_CASE("CommandShell mark advances with each print", "[shell][drain]") {
    NullLogger logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);

    shell.print("a");
    shell.print("b");
    shell.print("c");
    CHECK(shell.mark() == 3);
}

TEST_CASE("CommandShell drainSince returns only new lines", "[shell][drain]") {
    NullLogger logger;
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
    NullLogger logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);

    shell.print("line");
    int m = shell.mark();

    CHECK(shell.drainSince(m).empty());
}

TEST_CASE("CommandShell drainSince successive calls with updated mark", "[shell][drain]") {
    NullLogger logger;
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
    NullLogger logger;
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
    NullLogger logger;
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
    NullLogger logger;
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

// ---------------------------------------------------------------------------
// The issuer-aware handler overload (#535)
// ---------------------------------------------------------------------------
//
// Added so a command whose OUTPUT is a durable record of an operator action -- a ban row's
// created_by -- can name who ran it. The plain handler shape stays for the ~43 registrations that
// do not care. What must hold is that the issuer arrives intact and that the capability gate is
// still applied first: a handler that sees the issuer must not be a handler that skipped the check.

TEST_CASE("CommandRegistry: an issuer-aware handler receives the dispatching issuer", "[registry][issuer]") {
    fl::CommandRegistry reg;
    uint32_t sawPeer = 0;
    fl::CapabilityMask sawCaps = 0;
    reg.registerCommand("whoami", "whoami", 0,
                        [&](std::span<std::string_view>, const fl::CommandIssuer& issuer) -> std::string {
                            sawPeer = issuer.peerId;
                            sawCaps = issuer.caps;
                            return "ok";
                        });

    fl::CommandIssuer peer;
    peer.peerId = 42;
    peer.caps = fl::kAdminCaps;
    CHECK(reg.dispatch("whoami", peer) == "ok");
    CHECK(sawPeer == 42u);
    CHECK(sawCaps == fl::kAdminCaps);

    // The system issuer is distinguishable from a peer, which is the whole distinction the audit
    // trail records: "console" versus a specific player.
    CHECK(reg.dispatch("whoami", fl::systemIssuer()) == "ok");
    CHECK(sawPeer == fl::kIssuerNoPeer);
}

TEST_CASE("CommandRegistry: an issuer-aware handler is still capability-gated", "[registry][issuer]") {
    // The failure worth guarding: a new handler shape that reached the body before the permission
    // check would be a privilege bypass introduced by writing the more convenient overload -- which
    // is exactly the mistake #1079 removed the issuer-less dispatch() to prevent.
    fl::CommandRegistry reg;
    bool ran = false;
    reg.registerCommand("privileged", "privileged", fl::capBit(fl::Capability::KickBan),
                        [&](std::span<std::string_view>, const fl::CommandIssuer&) -> std::string {
                            ran = true;
                            return "ran";
                        });

    fl::CommandIssuer unprivileged;
    unprivileged.peerId = 7;
    unprivileged.caps = 0;
    const std::string out = reg.dispatch("privileged", unprivileged);
    CHECK_FALSE(ran);
    CHECK(out.find("requires") != std::string::npos);

    // And it does run for an issuer who holds the capability.
    fl::CommandIssuer allowed;
    allowed.peerId = 8;
    allowed.caps = fl::capBit(fl::Capability::KickBan);
    CHECK(reg.dispatch("privileged", allowed) == "ran");
    CHECK(ran);
}

TEST_CASE("CommandRegistry: the two handler shapes coexist in one registry", "[registry][issuer]") {
    fl::CommandRegistry reg;
    reg.registerCommand("plain", "plain", 0, [](std::span<std::string_view>) -> std::string { return "plain"; });
    reg.registerCommand("aware", "aware", 0,
                        [](std::span<std::string_view>, const fl::CommandIssuer&) -> std::string { return "aware"; });

    CHECK(reg.dispatch("plain", fl::systemIssuer()) == "plain");
    CHECK(reg.dispatch("aware", fl::systemIssuer()) == "aware");
    // Both appear in help: an operator should not be able to tell which shape a command uses.
    CHECK(reg.helpText().find("plain") != std::string::npos);
    CHECK(reg.helpText().find("aware") != std::string::npos);
}
