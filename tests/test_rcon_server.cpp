// SPDX-License-Identifier: GPL-3.0-or-later
#include "RconServer.h"
#include "console/CommandRegistry.h"
#include "console/CommandShell.h"
#include "mock_hal.h"
#include "server_config.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// rcon::encodePacket
// ---------------------------------------------------------------------------

TEST_CASE("encodePacket produces correct wire bytes for empty body", "[rcon][encode]") {
    // AUTH_RESPONSE with id=5, empty body:
    // Wire: [size:4LE][id:4LE][type:4LE][NUL][NUL]
    // size = 10 (8 + 0 + 2)
    auto pkt = rcon::encodePacket(5, rcon::kTypeAuthResponse, "");
    REQUIRE(pkt.size() == 14);

    int32_t size = 0, id = 0, type = 0;
    std::memcpy(&size, pkt.data(), 4);
    std::memcpy(&id, pkt.data() + 4, 4);
    std::memcpy(&type, pkt.data() + 8, 4);

    CHECK(size == 10);
    CHECK(id == 5);
    CHECK(type == rcon::kTypeAuthResponse);
    CHECK(pkt[12] == 0); // body NUL
    CHECK(pkt[13] == 0); // trailing NUL
}

TEST_CASE("encodePacket produces correct wire bytes with body", "[rcon][encode]") {
    auto pkt = rcon::encodePacket(1, rcon::kTypeResponseValue, "hello");
    // size = 10 + 5 = 15; total = 4 + 15 = 19
    REQUIRE(pkt.size() == 19);

    int32_t size = 0;
    std::memcpy(&size, pkt.data(), 4);
    CHECK(size == 15);
    CHECK(std::memcmp(pkt.data() + 12, "hello", 5) == 0);
    CHECK(pkt[17] == 0); // body NUL
    CHECK(pkt[18] == 0); // trailing NUL
}

TEST_CASE("encodePacket id=-1 for auth failure", "[rcon][encode]") {
    auto pkt = rcon::encodePacket(-1, rcon::kTypeAuthResponse, "");
    REQUIRE(pkt.size() == 14);
    int32_t id = 0;
    std::memcpy(&id, pkt.data() + 4, 4);
    CHECK(id == -1);
}

// ---------------------------------------------------------------------------
// rcon::decodePacket
// ---------------------------------------------------------------------------

TEST_CASE("decodePacket round-trip", "[rcon][decode]") {
    auto encoded = rcon::encodePacket(42, rcon::kTypeExecCommand, "status");
    rcon::RconPacket out;
    int consumed = rcon::decodePacket(encoded.data(), static_cast<int>(encoded.size()), out);
    CHECK(consumed == static_cast<int>(encoded.size()));
    CHECK(out.id == 42);
    CHECK(out.type == rcon::kTypeExecCommand);
    CHECK(out.body == "status");
}

TEST_CASE("decodePacket returns 0 for partial buffer", "[rcon][decode]") {
    auto encoded = rcon::encodePacket(1, rcon::kTypeAuth, "pass");
    rcon::RconPacket out;
    // Only send 10 bytes of a 18-byte packet.
    int consumed = rcon::decodePacket(encoded.data(), 10, out);
    CHECK(consumed == 0);
}

TEST_CASE("decodePacket returns 0 when fewer than 4 bytes available", "[rcon][decode]") {
    auto encoded = rcon::encodePacket(1, rcon::kTypeAuth, "x");
    rcon::RconPacket out;
    CHECK(rcon::decodePacket(encoded.data(), 3, out) == 0);
}

TEST_CASE("decodePacket returns -1 for malformed size (too small)", "[rcon][decode]") {
    // Construct a packet with size=5 (below minimum of 10).
    std::vector<uint8_t> bad(14, 0);
    int32_t size = 5;
    std::memcpy(bad.data(), &size, 4);
    rcon::RconPacket out;
    CHECK(rcon::decodePacket(bad.data(), static_cast<int>(bad.size()), out) == -1);
}

TEST_CASE("decodePacket returns -1 for malformed size (too large)", "[rcon][decode]") {
    std::vector<uint8_t> bad(14, 0);
    int32_t size = 10 + rcon::kMaxBodyPerPacket + 1; // one byte over the cap
    std::memcpy(bad.data(), &size, 4);
    rcon::RconPacket out;
    CHECK(rcon::decodePacket(bad.data(), static_cast<int>(bad.size()), out) == -1);
}

TEST_CASE("decodePacket handles two packets in one buffer", "[rcon][decode]") {
    auto p1 = rcon::encodePacket(1, rcon::kTypeExecCommand, "help");
    auto p2 = rcon::encodePacket(2, rcon::kTypeExecCommand, "status");
    std::vector<uint8_t> combined;
    combined.insert(combined.end(), p1.begin(), p1.end());
    combined.insert(combined.end(), p2.begin(), p2.end());

    rcon::RconPacket out1, out2;
    int c1 = rcon::decodePacket(combined.data(), static_cast<int>(combined.size()), out1);
    REQUIRE(c1 == static_cast<int>(p1.size()));
    int c2 = rcon::decodePacket(combined.data() + c1, static_cast<int>(combined.size()) - c1, out2);
    REQUIRE(c2 == static_cast<int>(p2.size()));

    CHECK(out1.body == "help");
    CHECK(out2.body == "status");
}

TEST_CASE("decodePacket with body exactly kMaxBodyPerPacket bytes", "[rcon][decode]") {
    std::string bigBody(static_cast<std::size_t>(rcon::kMaxBodyPerPacket), 'x');
    auto encoded = rcon::encodePacket(7, rcon::kTypeResponseValue, bigBody);
    rcon::RconPacket out;
    int consumed = rcon::decodePacket(encoded.data(), static_cast<int>(encoded.size()), out);
    CHECK(consumed == static_cast<int>(encoded.size()));
    CHECK(out.body == bigBody);
}

// ---------------------------------------------------------------------------
// rcon::splitResponse
// ---------------------------------------------------------------------------

TEST_CASE("splitResponse returns single chunk for short body", "[rcon][split]") {
    auto chunks = rcon::splitResponse("hello world");
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0] == "hello world");
}

TEST_CASE("splitResponse returns single chunk for body at exact limit", "[rcon][split]") {
    std::string body(static_cast<std::size_t>(rcon::kMaxBodyPerPacket), 'a');
    auto chunks = rcon::splitResponse(body);
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0].size() == static_cast<std::size_t>(rcon::kMaxBodyPerPacket));
}

TEST_CASE("splitResponse splits body one byte over the limit", "[rcon][split]") {
    std::string body(static_cast<std::size_t>(rcon::kMaxBodyPerPacket) + 1, 'b');
    auto chunks = rcon::splitResponse(body);
    REQUIRE(chunks.size() == 2);
    CHECK(chunks[0].size() == static_cast<std::size_t>(rcon::kMaxBodyPerPacket));
    CHECK(chunks[1].size() == 1);
}

TEST_CASE("splitResponse returns one empty string for empty body", "[rcon][split]") {
    auto chunks = rcon::splitResponse("");
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0].empty());
}

// ---------------------------------------------------------------------------
// rcon::AuthTracker — per-IP failed-auth counter and lockout
// ---------------------------------------------------------------------------

using SteadyTp = std::chrono::steady_clock::time_point;

TEST_CASE("AuthTracker: counter increments, no lockout before threshold", "[rcon][auth_tracker]") {
    rcon::AuthTracker tracker(5, 60);
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(tracker.recordFailure("1.2.3.4"));
        CHECK_FALSE(tracker.isLockedOut("1.2.3.4"));
    }
}

TEST_CASE("AuthTracker: lockout triggered on Nth failure", "[rcon][auth_tracker]") {
    rcon::AuthTracker tracker(5, 60);
    for (int i = 0; i < 4; ++i)
        tracker.recordFailure("1.2.3.4");
    CHECK(tracker.recordFailure("1.2.3.4")); // 5th = lockout
    CHECK(tracker.isLockedOut("1.2.3.4"));
}

TEST_CASE("AuthTracker: isLockedOut false after expiry (clock override)", "[rcon][auth_tracker]") {
    rcon::AuthTracker tracker(5, 60);
    SteadyTp now{};
    tracker.setClockOverride([&now] { return now; });
    for (int i = 0; i < 5; ++i)
        tracker.recordFailure("1.2.3.4");
    CHECK(tracker.isLockedOut("1.2.3.4"));
    now += std::chrono::seconds(61);
    CHECK_FALSE(tracker.isLockedOut("1.2.3.4"));
}

TEST_CASE("AuthTracker: recordSuccess clears failure counter", "[rcon][auth_tracker]") {
    rcon::AuthTracker tracker(5, 60);
    for (int i = 0; i < 3; ++i)
        tracker.recordFailure("1.2.3.4");
    tracker.recordSuccess("1.2.3.4");
    // Counter reset to 0; 4 more failures stay below threshold
    for (int i = 0; i < 4; ++i)
        CHECK_FALSE(tracker.recordFailure("1.2.3.4"));
    // 5th since reset triggers lockout
    CHECK(tracker.recordFailure("1.2.3.4"));
}

TEST_CASE("AuthTracker: recordSuccess does not clear an active lockout", "[rcon][auth_tracker]") {
    rcon::AuthTracker tracker(5, 60);
    for (int i = 0; i < 5; ++i)
        tracker.recordFailure("1.2.3.4");
    CHECK(tracker.isLockedOut("1.2.3.4"));
    tracker.recordSuccess("1.2.3.4");
    CHECK(tracker.isLockedOut("1.2.3.4")); // lockout persists; only expiry clears it
}

TEST_CASE("AuthTracker: after lockout expiry failure counter restarts from zero", "[rcon][auth_tracker]") {
    rcon::AuthTracker tracker(5, 60);
    SteadyTp now{};
    tracker.setClockOverride([&now] { return now; });
    for (int i = 0; i < 5; ++i)
        tracker.recordFailure("1.2.3.4");
    now += std::chrono::seconds(61);
    CHECK_FALSE(tracker.isLockedOut("1.2.3.4")); // expired
    // Fresh counter: 4 failures stay below threshold
    for (int i = 0; i < 4; ++i)
        CHECK_FALSE(tracker.recordFailure("1.2.3.4"));
}

TEST_CASE("AuthTracker: multiple IPs tracked independently", "[rcon][auth_tracker]") {
    rcon::AuthTracker tracker(5, 60);
    for (int i = 0; i < 4; ++i) {
        tracker.recordFailure("10.0.0.1");
        tracker.recordFailure("10.0.0.2");
    }
    CHECK_FALSE(tracker.isLockedOut("10.0.0.1"));
    CHECK_FALSE(tracker.isLockedOut("10.0.0.2"));
    CHECK(tracker.recordFailure("10.0.0.1")); // 5th for IP A → lockout
    CHECK(tracker.isLockedOut("10.0.0.1"));
    CHECK_FALSE(tracker.isLockedOut("10.0.0.2")); // IP B unaffected
}

TEST_CASE("AuthTracker: failure counter persists across reconnects", "[rcon][auth_tracker]") {
    rcon::AuthTracker tracker(5, 60);
    for (int i = 0; i < 3; ++i)
        tracker.recordFailure("1.2.3.4");
    CHECK_FALSE(tracker.isLockedOut("1.2.3.4"));
    // Simulated reconnect without success: counter continues from 3
    CHECK_FALSE(tracker.recordFailure("1.2.3.4")); // 4th
    CHECK(tracker.recordFailure("1.2.3.4"));       // 5th → lockout
}

TEST_CASE("AuthTracker: pruneExpired removes expired entry", "[rcon][auth_tracker]") {
    rcon::AuthTracker tracker(5, 60);
    SteadyTp now{};
    tracker.setClockOverride([&now] { return now; });
    for (int i = 0; i < 5; ++i)
        tracker.recordFailure("1.2.3.4");
    now += std::chrono::seconds(61);
    tracker.pruneExpired();
    CHECK_FALSE(tracker.isLockedOut("1.2.3.4"));
}

// ---------------------------------------------------------------------------
// parseServerConfig [rcon] section
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig [rcon] defaults when section absent", "[rcon][config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nname = \"test\"\n", &log);
    CHECK_FALSE(cfg.rcon.enabled);
    CHECK(cfg.rcon.port == 27015);
    CHECK(cfg.rcon.password.empty());
    CHECK(cfg.rcon.maxAuthFailures == 5);
    CHECK(cfg.rcon.lockoutSeconds == 60);
}

TEST_CASE("parseServerConfig [rcon] reads all fields", "[rcon][config]") {
    MockLogger log;
    const char* toml = "[rcon]\nenabled = true\nport = 25575\npassword = \"s3cr3t\"\n";
    auto cfg = parseServerConfig(toml, &log);
    CHECK(cfg.rcon.enabled);
    CHECK(cfg.rcon.port == 25575);
    CHECK(cfg.rcon.password == "s3cr3t");
    CHECK_FALSE(log.hasMessage(LogLevel::Warn, "rcon"));
}

TEST_CASE("parseServerConfig [rcon] warns on out-of-range port", "[rcon][config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[rcon]\nport = 99999\n", &log);
    CHECK(cfg.rcon.port == 27015); // default unchanged
    CHECK(log.hasMessage(LogLevel::Warn, "rcon.port"));
}

TEST_CASE("parseServerConfig [rcon] warns when enabled with empty password", "[rcon][config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[rcon]\nenabled = true\npassword = \"\"\n", &log);
    CHECK(cfg.rcon.enabled);
    CHECK(log.hasMessage(LogLevel::Warn, "rcon.password"));
}

TEST_CASE("parseServerConfig [rcon] no warning when enabled with non-empty password", "[rcon][config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[rcon]\nenabled = true\npassword = \"strongpass\"\n", &log);
    CHECK(cfg.rcon.enabled);
    CHECK_FALSE(log.hasMessage(LogLevel::Warn, "rcon.password"));
}

TEST_CASE("parseServerConfig [rcon] reads max_auth_failures", "[rcon][config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[rcon]\nmax_auth_failures = 3\n", &log);
    CHECK(cfg.rcon.maxAuthFailures == 3);
    CHECK_FALSE(log.hasMessage(LogLevel::Warn, "rcon.max_auth_failures"));
}

TEST_CASE("parseServerConfig [rcon] reads lockout_seconds", "[rcon][config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[rcon]\nlockout_seconds = 120\n", &log);
    CHECK(cfg.rcon.lockoutSeconds == 120);
    CHECK_FALSE(log.hasMessage(LogLevel::Warn, "rcon.lockout_seconds"));
}

TEST_CASE("parseServerConfig [rcon] warns on out-of-range max_auth_failures", "[rcon][config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[rcon]\nmax_auth_failures = 9999\n", &log);
    CHECK(cfg.rcon.maxAuthFailures == 5); // default unchanged
    CHECK(log.hasMessage(LogLevel::Warn, "rcon.max_auth_failures"));
}

TEST_CASE("parseServerConfig [rcon] warns on out-of-range lockout_seconds", "[rcon][config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[rcon]\nlockout_seconds = 0\n", &log);
    CHECK(cfg.rcon.lockoutSeconds == 60); // default unchanged
    CHECK(log.hasMessage(LogLevel::Warn, "rcon.lockout_seconds"));
}

TEST_CASE("parseServerConfig [rcon] max_auth_failures at max boundary is valid", "[rcon][config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[rcon]\nmax_auth_failures = 1000\n", &log);
    CHECK(cfg.rcon.maxAuthFailures == 1000);
    CHECK_FALSE(log.hasMessage(LogLevel::Warn, "rcon.max_auth_failures"));
}

TEST_CASE("parseServerConfig [rcon] lockout_seconds at max boundary is valid", "[rcon][config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[rcon]\nlockout_seconds = 86400\n", &log);
    CHECK(cfg.rcon.lockoutSeconds == 86400);
    CHECK_FALSE(log.hasMessage(LogLevel::Warn, "rcon.lockout_seconds"));
}

// ---------------------------------------------------------------------------
// RCON drain path: drainSince + encodePacket/splitResponse/decodePacket
// ---------------------------------------------------------------------------

TEST_CASE("RCON drain: drainSince plus encodePacket produces valid RESPONSE_VALUE", "[rcon][drain]") {
    MockLogger log;
    CommandRegistry reg;
    CommandShell shell(log, reg);

    int m = shell.mark();
    // Simulate async sim-callback confirmations written after dispatch()
    shell.print("[admin] kicked peer 1");
    shell.print("[admin] banned 192.168.1.10");

    auto lines = shell.drainSince(m);
    REQUIRE(lines.size() == 2);

    std::string combined;
    for (const auto& l : lines) {
        if (!combined.empty())
            combined += '\n';
        combined += l;
    }

    auto chunks = rcon::splitResponse(combined);
    REQUIRE(chunks.size() == 1); // both lines fit in one chunk
    auto pkt = rcon::encodePacket(7, rcon::kTypeResponseValue, chunks[0]);

    rcon::RconPacket decoded;
    int consumed = rcon::decodePacket(pkt.data(), static_cast<int>(pkt.size()), decoded);
    REQUIRE(consumed == static_cast<int>(pkt.size()));
    CHECK(decoded.id == 7);
    CHECK(decoded.type == rcon::kTypeResponseValue);
    CHECK(decoded.body.find("kicked peer 1") != std::string::npos);
    CHECK(decoded.body.find("banned 192.168.1.10") != std::string::npos);
}

TEST_CASE("RCON drain: empty when no async output since mark", "[rcon][drain]") {
    MockLogger log;
    CommandRegistry reg;
    CommandShell shell(log, reg);

    shell.print("before");
    int m = shell.mark();

    // No new writes after mark
    auto lines = shell.drainSince(m);
    CHECK(lines.empty());
    // No packets would be generated — verify splitResponse on empty string gives one empty chunk
    // (so callers safely detect no-op via lines.empty() check before encoding)
}

TEST_CASE("RCON drain: multi-line output stays within single kMaxBodyPerPacket chunk", "[rcon][drain]") {
    MockLogger log;
    CommandRegistry reg;
    CommandShell shell(log, reg);

    int m = shell.mark();
    // 10 typical confirmation lines (~80 chars each = ~800 bytes total, well under 4086)
    for (int i = 0; i < 10; ++i)
        shell.print("[admin] peer " + std::to_string(i) + " addr 192.168.1." + std::to_string(i) +
                    " entity=12/3 confirmed");

    auto lines = shell.drainSince(m);
    REQUIRE(lines.size() == 10);

    std::string combined;
    for (const auto& l : lines) {
        if (!combined.empty())
            combined += '\n';
        combined += l;
    }
    CHECK(combined.size() < static_cast<std::size_t>(rcon::kMaxBodyPerPacket));

    auto chunks = rcon::splitResponse(combined);
    CHECK(chunks.size() == 1); // all lines fit in a single packet
}
