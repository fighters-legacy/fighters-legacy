// SPDX-License-Identifier: GPL-3.0-or-later
//
// server_shutdown_stdin_open (#1038) — the regression test for the shutdown deadlock.
//
// fl-server used to hang forever on `quit` (and on Ctrl-C) whenever its parent kept stdin OPEN: a
// detached reader thread sat in std::getline holding the stdin FILE lock, and exit()'s _IO_cleanup
// blocked taking that same lock to flush. The process was already past the end of main().
//
// The reason it survived every existing smoke test is the whole point of this file: piping input
// (`printf 'quit\n' | fl-server`) CLOSES stdin, getline returns, and the process exits cleanly. Only
// a parent that holds the pipe open reproduces it -- an interactive terminal, `docker run -i`, or
// exactly what Subprocess::spawn(captureStdin=true) does, which is also how the game client starts
// its single-player server. So this test spawns the real binary the way the real client does.
//
// Runs one server on one port in one TEST_CASE deliberately: catch_discover_tests registers each
// case as its own ctest, and ctest may run them in parallel, which two servers on one port would
// not survive.

#include "Subprocess.h"

#include <StdoutLogger.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

// A port well outside the default 4778/4779 range so a developer's running server or an unrelated
// CI job cannot collide with it.
constexpr const char* kTestPort = "47791";

constexpr auto kStartupDeadline = std::chrono::seconds(30);
// The bug: this deadline was infinite. 15 s is ~50x the honest shutdown time of an idle server.
constexpr auto kExitDeadline = std::chrono::seconds(15);

} // namespace

TEST_CASE("fl-server exits on quit while stdin stays open", "[server][shutdown]") {
    fl::StdoutLogger log;

    // enet6 + a single sim worker: the leanest startup that is still the real server. --transport
    // enet matches what LocalServer spawns for single-player, and keeps the test valid in the
    // FL_ENABLE_GNS=OFF build legs.
    const std::vector<std::string> args{kTestPort, "1",           "--bind", "127.0.0.1", "--sim-worker-threads",
                                        "1",       "--transport", "enet"};

    fl::Subprocess server = fl::Subprocess::spawn(FL_SERVER_BIN, args, /*captureStdout=*/true,
                                                  /*captureStdin=*/true, log);
    REQUIRE(server.valid());

    // Wait for the server to report it is up. Without this the test could "pass" against a server
    // that died on a bind failure and was never running in the first place.
    bool listening = false;
    const auto startDeadline = std::chrono::steady_clock::now() + kStartupDeadline;
    while (!listening && std::chrono::steady_clock::now() < startDeadline) {
        const auto line = server.readStdoutLine(1000);
        if (!line)
            continue;
        if (line->find("listening on") != std::string::npos)
            listening = true;
    }
    REQUIRE(listening);
    REQUIRE(server.isRunning());

    // stdin is still held open by this process -- the condition the deadlock needs.
    server.writeStdin("quit");

    bool exited = false;
    const auto exitDeadline = std::chrono::steady_clock::now() + kExitDeadline;
    while (std::chrono::steady_clock::now() < exitDeadline) {
        if (!server.isRunning()) {
            exited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Fails on main as of #1037: the server logs "shutting down", returns from main(), and then
    // hangs inside exit().
    CHECK(exited);

    if (!exited)
        server.stop(); // do not leave a wedged server behind for the next test run
}
