// SPDX-License-Identifier: GPL-3.0-or-later
//
// StdinCommandReader / LineAssembler (#1038) — the admin console's stdin line source.
//
// The bug this replaces was a DEADLOCK, not a parsing defect: a detached thread blocked in
// std::getline held the C-stdio lock that exit()'s flush waits on, so fl-server never exited while
// stdin stayed open. The integration ctest `server_shutdown_stdin_open` covers the deadlock end to
// end (spawn with stdin held open, send `quit`, assert exit). What is tested here is everything
// that used to be free by virtue of using getline and now has to be earned: line splitting across
// arbitrary read boundaries, and a reader lifecycle that survives being stopped from any state.

#include "StdinCommandReader.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using fl::LineAssembler;
using fl::StdinCommandReader;

namespace {

std::vector<std::string> feedAll(LineAssembler& a, const std::string& chunk) {
    std::vector<std::string> out;
    a.feed(chunk.data(), chunk.size(), out);
    return out;
}

} // namespace

TEST_CASE("LineAssembler splits a simple line", "[stdin]") {
    LineAssembler a;
    const auto lines = feedAll(a, "status\n");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "status");
    CHECK(a.pendingBytes() == 0);
}

TEST_CASE("LineAssembler buffers an unterminated chunk", "[stdin]") {
    LineAssembler a;
    auto lines = feedAll(a, "stat");
    CHECK(lines.empty());
    CHECK(a.pendingBytes() == 4);

    // The rest of the line arrives in a later read -- the case a per-read parse would drop.
    lines = feedAll(a, "us\n");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "status");
    CHECK(a.pendingBytes() == 0);
}

TEST_CASE("LineAssembler handles several lines in one read", "[stdin]") {
    LineAssembler a;
    const auto lines = feedAll(a, "peers\nstatus\ntickstats\n");
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "peers");
    CHECK(lines[1] == "status");
    CHECK(lines[2] == "tickstats");
}

TEST_CASE("LineAssembler strips CR from CRLF input", "[stdin]") {
    LineAssembler a;
    // A Windows client, or a file with CRLF endings piped in. A trailing '\r' would make every
    // command an unknown command.
    const auto lines = feedAll(a, "status\r\npeers\r\n");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "status");
    CHECK(lines[1] == "peers");
}

TEST_CASE("LineAssembler emits empty lines", "[stdin]") {
    LineAssembler a;
    // A bare Enter dispatches an empty command (which the registry rejects) -- the same as before.
    const auto lines = feedAll(a, "\n\nstatus\n");
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].empty());
    CHECK(lines[1].empty());
    CHECK(lines[2] == "status");
}

TEST_CASE("LineAssembler splits a line arriving one byte at a time", "[stdin]") {
    LineAssembler a;
    std::vector<std::string> out;
    const std::string input = "kick 3\n";
    for (char c : input)
        a.feed(&c, 1, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "kick 3");
}

TEST_CASE("LineAssembler caps an overlong line and resynchronises", "[stdin]") {
    LineAssembler a;
    const std::string huge(LineAssembler::kMaxLineBytes + 5000, 'x');
    std::vector<std::string> out;
    a.feed(huge.data(), huge.size(), out);

    // The capped prefix is emitted once; the tail is discarded rather than buffered without bound.
    REQUIRE(out.size() == 1);
    CHECK(out[0].size() == LineAssembler::kMaxLineBytes);
    CHECK(a.pendingBytes() == 0);

    // The terminator ends the discarded tail, and the NEXT line parses normally -- an overlong
    // line must not poison the stream behind it.
    out.clear();
    a.feed("\nstatus\n", 8, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "status");
}

TEST_CASE("LineAssembler flush emits a trailing unterminated line at EOF", "[stdin]") {
    LineAssembler a;
    std::vector<std::string> out;
    a.feed("quit", 4, out);
    CHECK(out.empty());

    a.flush(out);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "quit");

    // Idempotent: a second flush emits nothing.
    out.clear();
    a.flush(out);
    CHECK(out.empty());
}

TEST_CASE("LineAssembler flush strips a trailing CR", "[stdin]") {
    LineAssembler a;
    std::vector<std::string> out;
    a.feed("quit\r", 5, out);
    a.flush(out);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "quit");
}

TEST_CASE("LineAssembler flush after an overlong line emits nothing", "[stdin]") {
    LineAssembler a;
    const std::string huge(LineAssembler::kMaxLineBytes + 10, 'x');
    std::vector<std::string> out;
    a.feed(huge.data(), huge.size(), out);
    out.clear();

    a.flush(out); // the discarded tail is not a line
    CHECK(out.empty());
}

TEST_CASE("StdinCommandReader is safe before start and after stop", "[stdin]") {
    StdinCommandReader reader;
    std::vector<std::string> lines;

    // Never started: draining yields nothing, eof() is false, stop() is a no-op.
    reader.drain(lines);
    CHECK(lines.empty());
    CHECK_FALSE(reader.eof());
    reader.stop();
    reader.stop(); // idempotent

    reader.drain(lines);
    CHECK(lines.empty());
}

TEST_CASE("StdinCommandReader start/stop joins its thread", "[stdin]") {
    // The property the deadlock was about: a started reader can always be stopped, and stopping it
    // returns promptly (bounded by kPollTimeoutMs, not by whether anyone ever types anything).
    StdinCommandReader reader;
    reader.start();
    reader.start(); // idempotent -- must not spawn a second thread over the first
    reader.stop();

    std::vector<std::string> lines;
    reader.drain(lines);
    CHECK(lines.empty());
}

TEST_CASE("StdinCommandReader restarts after a stop", "[stdin]") {
    StdinCommandReader reader;
    reader.start();
    reader.stop();
    reader.start();
    reader.stop();
    CHECK_FALSE(reader.eof()); // state was released with the previous run
}
