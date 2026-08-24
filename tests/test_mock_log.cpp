// SPDX-License-Identifier: GPL-3.0-or-later
#include "mock_log.h"

#include <catch2/catch_test_macros.hpp>

using namespace fl;

// The shared doubles replaced 47 hand-rolled loggers, four of which filtered to a single level on
// the way IN (a warnings-only vector). RecordingLogger records every level and filters on the way
// OUT instead, so these cases pin the queries the converted suites now depend on.

TEST_CASE("NullLogger swallows everything and stays usable", "[mock_log]") {
    NullLogger log;
    ILogger& iface = log;
    iface.log(LogLevel::Error, __FILE__, __LINE__, "boom");
    iface.setMinLevel(LogLevel::Warn);
    iface.flush();
    SUCCEED("no state to observe -- the point is that it compiles and does not crash");
}

TEST_CASE("RecordingLogger keeps every level, in order", "[mock_log]") {
    RecordingLogger log;
    log.log(LogLevel::Warn, "f", 1, "first warning");
    log.log(LogLevel::Error, "f", 2, "an error");
    log.log(LogLevel::Warn, "f", 3, "second warning");

    REQUIRE(log.entries.size() == 3);
    CHECK(log.entries[0].level == LogLevel::Warn);
    CHECK(log.entries[0].message == "first warning");
    CHECK(log.entries[1].message == "an error");
}

TEST_CASE("RecordingLogger count with no needle counts the whole level", "[mock_log]") {
    RecordingLogger log;
    log.log(LogLevel::Warn, "f", 1, "alpha");
    log.log(LogLevel::Warn, "f", 2, "beta");
    log.log(LogLevel::Error, "f", 3, "alpha");

    CHECK(log.count(LogLevel::Warn) == 2);
    CHECK(log.count(LogLevel::Error) == 1);
    CHECK(log.count(LogLevel::Info) == 0);
    CHECK(log.count(LogLevel::Warn, "alpha") == 1); // the needle narrows within the level
    CHECK(log.count(LogLevel::Warn, "nope") == 0);
}

TEST_CASE("RecordingLogger hasMessage matches a substring at one level only", "[mock_log]") {
    RecordingLogger log;
    log.log(LogLevel::Error, "f", 1, "unknown sensor def id 'fl-base:typo'");

    CHECK(log.hasMessage(LogLevel::Error, "unknown sensor def id"));
    CHECK(log.hasMessage(LogLevel::Error)); // no needle -- "anything at this level"
    CHECK_FALSE(log.hasMessage(LogLevel::Warn, "unknown sensor def id"));
    CHECK_FALSE(log.hasMessage(LogLevel::Warn));
}

TEST_CASE("RecordingLogger messages returns one level in order", "[mock_log]") {
    RecordingLogger log;
    log.log(LogLevel::Warn, "f", 1, "w1");
    log.log(LogLevel::Error, "f", 2, "e1");
    log.log(LogLevel::Warn, "f", 3, "w2");

    const auto warns = log.messages(LogLevel::Warn);
    REQUIRE(warns.size() == 2);
    CHECK(warns[0] == "w1");
    CHECK(warns[1] == "w2");
    CHECK(log.messages(LogLevel::Trace).empty());
}

TEST_CASE("RecordingLogger tolerates a null message", "[mock_log]") {
    // ILogger takes const char*, and the hand-rolled copies were split on whether they guarded it.
    // Constructing std::string from nullptr is undefined, so the shared recorder guards.
    RecordingLogger log;
    log.log(LogLevel::Warn, "f", 1, nullptr);

    REQUIRE(log.entries.size() == 1);
    CHECK(log.entries[0].message.empty());
    CHECK(log.count(LogLevel::Warn) == 1);
}
