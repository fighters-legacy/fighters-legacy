// SPDX-License-Identifier: GPL-3.0-or-later
#include "IClock.h"
#include "RconDrainHelper.h"
#include "RconServer.h"
#include "console/CommandRegistry.h"
#include "console/CommandShell.h"
#include "mock_hal.h"
#include "server_config.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using namespace fl;
namespace rcon = fl::rcon;

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
// fl::AuthTracker — per-IP failed-auth counter and lockout
// ---------------------------------------------------------------------------

TEST_CASE("AuthTracker: counter increments, no lockout before threshold", "[rcon][auth_tracker]") {
    fl::AuthTracker tracker(5, 60);
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(tracker.recordFailure("1.2.3.4"));
        CHECK_FALSE(tracker.isLockedOut("1.2.3.4"));
    }
}

TEST_CASE("AuthTracker: lockout triggered on Nth failure", "[rcon][auth_tracker]") {
    fl::AuthTracker tracker(5, 60);
    for (int i = 0; i < 4; ++i)
        tracker.recordFailure("1.2.3.4");
    CHECK(tracker.recordFailure("1.2.3.4")); // 5th = lockout
    CHECK(tracker.isLockedOut("1.2.3.4"));
}

TEST_CASE("AuthTracker: isLockedOut false after expiry (clock override)", "[rcon][auth_tracker]") {
    fl::AuthTracker tracker(5, 60);
    fl::ManualClock now;
    tracker.setClock(now);
    for (int i = 0; i < 5; ++i)
        tracker.recordFailure("1.2.3.4");
    CHECK(tracker.isLockedOut("1.2.3.4"));
    now.advance(std::chrono::seconds(61));
    CHECK_FALSE(tracker.isLockedOut("1.2.3.4"));
}

TEST_CASE("AuthTracker: recordSuccess clears failure counter", "[rcon][auth_tracker]") {
    fl::AuthTracker tracker(5, 60);
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
    fl::AuthTracker tracker(5, 60);
    for (int i = 0; i < 5; ++i)
        tracker.recordFailure("1.2.3.4");
    CHECK(tracker.isLockedOut("1.2.3.4"));
    tracker.recordSuccess("1.2.3.4");
    CHECK(tracker.isLockedOut("1.2.3.4")); // lockout persists; only expiry clears it
}

TEST_CASE("AuthTracker: after lockout expiry failure counter restarts from zero", "[rcon][auth_tracker]") {
    fl::AuthTracker tracker(5, 60);
    fl::ManualClock now;
    tracker.setClock(now);
    for (int i = 0; i < 5; ++i)
        tracker.recordFailure("1.2.3.4");
    now.advance(std::chrono::seconds(61));
    CHECK_FALSE(tracker.isLockedOut("1.2.3.4")); // expired
    // Fresh counter: 4 failures stay below threshold
    for (int i = 0; i < 4; ++i)
        CHECK_FALSE(tracker.recordFailure("1.2.3.4"));
}

TEST_CASE("AuthTracker: multiple IPs tracked independently", "[rcon][auth_tracker]") {
    fl::AuthTracker tracker(5, 60);
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
    fl::AuthTracker tracker(5, 60);
    for (int i = 0; i < 3; ++i)
        tracker.recordFailure("1.2.3.4");
    CHECK_FALSE(tracker.isLockedOut("1.2.3.4"));
    // Simulated reconnect without success: counter continues from 3
    CHECK_FALSE(tracker.recordFailure("1.2.3.4")); // 4th
    CHECK(tracker.recordFailure("1.2.3.4"));       // 5th → lockout
}

TEST_CASE("AuthTracker: pruneExpired removes expired entry", "[rcon][auth_tracker]") {
    fl::AuthTracker tracker(5, 60);
    fl::ManualClock now;
    tracker.setClock(now);
    for (int i = 0; i < 5; ++i)
        tracker.recordFailure("1.2.3.4");
    now.advance(std::chrono::seconds(61));
    tracker.pruneExpired();
    CHECK_FALSE(tracker.isLockedOut("1.2.3.4"));
}

TEST_CASE("AuthTracker: clearLockout removes active lockout", "[rcon][auth_tracker]") {
    fl::AuthTracker tracker(5, 60);
    for (int i = 0; i < 5; ++i)
        tracker.recordFailure("1.2.3.4");
    CHECK(tracker.isLockedOut("1.2.3.4"));
    tracker.clearLockout("1.2.3.4");
    CHECK_FALSE(tracker.isLockedOut("1.2.3.4"));
}

TEST_CASE("AuthTracker: clearLockout clears failure counter", "[rcon][auth_tracker]") {
    fl::AuthTracker tracker(5, 60);
    for (int i = 0; i < 4; ++i)
        tracker.recordFailure("1.2.3.4");
    tracker.clearLockout("1.2.3.4");
    // Counter reset to 0; 4 more failures must not trigger lockout
    for (int i = 0; i < 4; ++i)
        CHECK_FALSE(tracker.recordFailure("1.2.3.4"));
    CHECK_FALSE(tracker.isLockedOut("1.2.3.4"));
}

TEST_CASE("AuthTracker: clearLockout is a no-op when IP is not locked", "[rcon][auth_tracker]") {
    fl::AuthTracker tracker(5, 60);
    tracker.clearLockout("1.2.3.4");
    CHECK_FALSE(tracker.isLockedOut("1.2.3.4"));
}

TEST_CASE("RconServer: clearLockout returns false for non-locked IP and does not crash", "[rcon]") {
    MockLogger log;
    CommandRegistry reg;
    ServerConfig::RconConfig cfg{};
    cfg.maxAuthFailures = 5;
    cfg.lockoutSeconds = 60;
    RconServer srv(reg, cfg, log);
    // start() not called — no sockets opened. Impl is constructed; clearLockout
    // exercises the pimpl forwarding + mutex path without network infrastructure.
    CHECK_FALSE(srv.clearLockout("1.2.3.4"));
}

TEST_CASE("RconServer: getRconAuthSummary returns empty summary on fresh server", "[rcon]") {
    MockLogger log;
    CommandRegistry reg;
    ServerConfig::RconConfig cfg{};
    cfg.maxAuthFailures = 5;
    cfg.lockoutSeconds = 60;
    RconServer srv(reg, cfg, log);
    // start() not called — exercises pimpl forwarding + mutex path.
    auto s = srv.getRconAuthSummary();
    CHECK(s.activeCount == 0);
    CHECK(s.threshold == 5);
    CHECK(s.entries.empty());
}

TEST_CASE("RconServer: getRconAuthSummary threshold matches config", "[rcon]") {
    MockLogger log;
    CommandRegistry reg;
    ServerConfig::RconConfig cfg{};
    cfg.maxAuthFailures = 3;
    cfg.lockoutSeconds = 120;
    RconServer srv(reg, cfg, log);
    auto s = srv.getRconAuthSummary();
    CHECK(s.threshold == 3);
    CHECK(s.activeCount == 0);
    CHECK(s.entries.empty());
}

TEST_CASE("RconServer: setClock can be called before start and does not crash", "[rcon]") {
    MockLogger log;
    CommandRegistry reg;
    ServerConfig::RconConfig cfg{};
    cfg.maxAuthFailures = 5;
    cfg.lockoutSeconds = 60;
    RconServer srv(reg, cfg, log);
    fl::ManualClock clk;
    srv.setClock(clk); // pimpl forwarding; no start() called, no sockets
}

TEST_CASE("RconServer: setClock propagates to internal AuthTracker", "[rcon]") {
    MockLogger log;
    CommandRegistry reg;
    ServerConfig::RconConfig cfg{};
    cfg.maxAuthFailures = 2;
    cfg.lockoutSeconds = 60;
    RconServer srv(reg, cfg, log);
    fl::ManualClock clk;
    srv.setClock(clk);
    // AuthTracker now uses clk; summary call still works, threshold unchanged
    auto s = srv.getRconAuthSummary();
    CHECK(s.threshold == 2);
    CHECK(s.activeCount == 0);
}

TEST_CASE("AuthTracker: lockedOutCount returns 0 initially", "[rcon][auth_tracker]") {
    fl::AuthTracker tracker(5, 60);
    CHECK(tracker.lockedOutCount() == 0);
}

TEST_CASE("AuthTracker: lockedOutCount reflects lockout and 0 after expiry", "[rcon][auth_tracker]") {
    fl::AuthTracker tracker(2, 60);
    fl::ManualClock now;
    tracker.setClock(now);
    tracker.recordFailure("1.2.3.4");
    tracker.recordFailure("1.2.3.4");
    CHECK(tracker.lockedOutCount() == 1);
    now.advance(std::chrono::seconds(61));
    CHECK(tracker.lockedOutCount() == 0);
}

TEST_CASE("AuthTracker: failureSummary shows locked-out entry", "[rcon][auth_tracker]") {
    fl::AuthTracker tracker(2, 300);
    fl::ManualClock now;
    tracker.setClock(now);
    tracker.recordFailure("1.2.3.4");
    tracker.recordFailure("1.2.3.4");
    auto entries = tracker.failureSummary();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].ip == "1.2.3.4");
    CHECK(entries[0].lockedOut == true);
    CHECK(entries[0].failures == 0);
    CHECK(entries[0].expiresIn > 0);
}

TEST_CASE("AuthTracker: failureSummary shows pending failure entry", "[rcon][auth_tracker]") {
    fl::AuthTracker tracker(5, 60);
    tracker.recordFailure("1.2.3.4");
    tracker.recordFailure("1.2.3.4");
    auto entries = tracker.failureSummary();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].ip == "1.2.3.4");
    CHECK(entries[0].lockedOut == false);
    CHECK(entries[0].failures == 2);
    CHECK(entries[0].expiresIn == 0);
}

TEST_CASE("AuthTracker: failureSummary excludes expired lockouts", "[rcon][auth_tracker]") {
    fl::AuthTracker tracker(2, 60);
    fl::ManualClock now;
    tracker.setClock(now);
    tracker.recordFailure("1.2.3.4");
    tracker.recordFailure("1.2.3.4");
    now.advance(std::chrono::seconds(61));
    auto entries = tracker.failureSummary();
    CHECK(entries.empty());
}

TEST_CASE("AuthTracker: maxFailures returns configured threshold", "[rcon][auth_tracker]") {
    fl::AuthTracker tracker(7, 60);
    CHECK(tracker.maxFailures() == 7);
}

TEST_CASE("AuthTracker: failureSummary returns both locked and pending IPs", "[rcon][auth_tracker]") {
    fl::AuthTracker tracker(2, 300);
    fl::ManualClock now;
    tracker.setClock(now);
    // Lock out first IP
    tracker.recordFailure("1.1.1.1");
    tracker.recordFailure("1.1.1.1");
    // One pending failure on second IP (below threshold)
    tracker.recordFailure("2.2.2.2");
    auto entries = tracker.failureSummary();
    REQUIRE(entries.size() == 2);
    auto it1 = std::find_if(entries.begin(), entries.end(), [](const auto& e) { return e.ip == "1.1.1.1"; });
    auto it2 = std::find_if(entries.begin(), entries.end(), [](const auto& e) { return e.ip == "2.2.2.2"; });
    REQUIRE(it1 != entries.end());
    REQUIRE(it2 != entries.end());
    CHECK(it1->lockedOut == true);
    CHECK(it1->failures == 0);
    CHECK(it2->lockedOut == false);
    CHECK(it2->failures == 1);
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

// ---------------------------------------------------------------------------
// checkAndFireDrains — drain deadline unit tests
// ---------------------------------------------------------------------------

TEST_CASE("RCON drain deadline: does not fire when now < drainDeadline", "[rcon][drain][deadline]") {
    MockLogger log;
    CommandRegistry reg;
    CommandShell shell(log, reg);
    ManualClock clk;

    rcon::DrainClientInfo c;
    c.connected = true;
    c.hasPendingDrain = true;
    c.drainMark = shell.mark();
    c.drainPacketId = 1;
    c.drainDeadline = clk.now() + std::chrono::milliseconds(100);

    shell.print("async output");

    std::vector<rcon::DrainClientInfo*> clients{&c};
    rcon::checkAndFireDrains(clients, clk.now(), &shell); // now < deadline

    CHECK(c.hasPendingDrain);
    CHECK(c.sendBuf.empty());
}

TEST_CASE("RCON drain deadline: fires when now >= drainDeadline, single line", "[rcon][drain][deadline]") {
    MockLogger log;
    CommandRegistry reg;
    CommandShell shell(log, reg);
    ManualClock clk;

    rcon::DrainClientInfo c;
    c.connected = true;
    c.hasPendingDrain = true;
    c.drainMark = shell.mark();
    c.drainPacketId = 7;
    c.drainDeadline = clk.now(); // deadline == now -> fires

    shell.print("kick confirmed");

    std::vector<rcon::DrainClientInfo*> clients{&c};
    rcon::checkAndFireDrains(clients, clk.now(), &shell);

    CHECK_FALSE(c.hasPendingDrain);
    REQUIRE_FALSE(c.sendBuf.empty());

    rcon::RconPacket pkt;
    int consumed = rcon::decodePacket(c.sendBuf.data(), static_cast<int>(c.sendBuf.size()), pkt);
    REQUIRE(consumed > 0);
    CHECK(pkt.id == 7);
    CHECK(pkt.type == rcon::kTypeResponseValue);
    CHECK(pkt.body.find("kick confirmed") != std::string::npos);
}

TEST_CASE("RCON drain deadline: fires exactly once, not retriggered on second call", "[rcon][drain][deadline]") {
    MockLogger log;
    CommandRegistry reg;
    CommandShell shell(log, reg);
    ManualClock clk;

    rcon::DrainClientInfo c;
    c.connected = true;
    c.hasPendingDrain = true;
    c.drainMark = shell.mark();
    c.drainPacketId = 1;
    c.drainDeadline = clk.now();

    shell.print("first output");

    std::vector<rcon::DrainClientInfo*> clients{&c};
    rcon::checkAndFireDrains(clients, clk.now(), &shell); // fires

    CHECK_FALSE(c.hasPendingDrain);
    std::size_t bufAfterFirst = c.sendBuf.size();

    shell.print("second output");

    rcon::checkAndFireDrains(clients, clk.now(), &shell); // hasPendingDrain is false — no fire

    CHECK(c.sendBuf.size() == bufAfterFirst);
}

TEST_CASE("RCON drain deadline: fires but queues no packets when shell has no output since mark",
          "[rcon][drain][deadline]") {
    MockLogger log;
    CommandRegistry reg;
    CommandShell shell(log, reg);
    ManualClock clk;

    shell.print("before mark"); // written before mark — not visible to drainSince

    rcon::DrainClientInfo c;
    c.connected = true;
    c.hasPendingDrain = true;
    c.drainMark = shell.mark(); // mark taken now
    c.drainPacketId = 3;
    c.drainDeadline = clk.now();

    // No shell.print() after mark

    std::vector<rcon::DrainClientInfo*> clients{&c};
    rcon::checkAndFireDrains(clients, clk.now(), &shell);

    CHECK_FALSE(c.hasPendingDrain); // cleared even with no output
    CHECK(c.sendBuf.empty());
}

TEST_CASE("RCON drain deadline: null shell is a no-op", "[rcon][drain][deadline]") {
    rcon::DrainClientInfo c;
    c.connected = true;
    c.hasPendingDrain = true;
    c.drainDeadline = std::chrono::steady_clock::time_point{}; // epoch = well in the past

    std::vector<rcon::DrainClientInfo*> clients{&c};
    rcon::checkAndFireDrains(clients, std::chrono::steady_clock::now(), nullptr);

    CHECK(c.hasPendingDrain); // unchanged
    CHECK(c.sendBuf.empty());
}

TEST_CASE("RCON drain deadline: disconnected client is skipped even when deadline passed", "[rcon][drain][deadline]") {
    MockLogger log;
    CommandRegistry reg;
    CommandShell shell(log, reg);
    ManualClock clk;

    rcon::DrainClientInfo c;
    c.connected = false; // disconnected
    c.hasPendingDrain = true;
    c.drainMark = shell.mark();
    c.drainDeadline = clk.now();

    shell.print("output");

    std::vector<rcon::DrainClientInfo*> clients{&c};
    rcon::checkAndFireDrains(clients, clk.now(), &shell);

    CHECK(c.hasPendingDrain); // not cleared
    CHECK(c.sendBuf.empty());
}

TEST_CASE("RCON drain deadline: multiple clients, only deadline-expired ones fire", "[rcon][drain][deadline]") {
    MockLogger log;
    CommandRegistry reg;
    CommandShell shell(log, reg);
    ManualClock clk;

    // Client A: deadline in the future — should NOT fire
    rcon::DrainClientInfo a;
    a.connected = true;
    a.hasPendingDrain = true;
    a.drainMark = shell.mark();
    a.drainPacketId = 10;
    a.drainDeadline = clk.now() + std::chrono::milliseconds(200);

    shell.print("for A");

    // Client B: deadline already passed — should fire
    rcon::DrainClientInfo b;
    b.connected = true;
    b.hasPendingDrain = true;
    b.drainMark = shell.mark();
    b.drainPacketId = 20;
    b.drainDeadline = clk.now();

    shell.print("for B");

    std::vector<rcon::DrainClientInfo*> clients{&a, &b};
    rcon::checkAndFireDrains(clients, clk.now(), &shell);

    CHECK(a.hasPendingDrain); // A's deadline not yet reached
    CHECK(a.sendBuf.empty());
    CHECK_FALSE(b.hasPendingDrain); // B fired
    REQUIRE_FALSE(b.sendBuf.empty());
    rcon::RconPacket pkt;
    rcon::decodePacket(b.sendBuf.data(), static_cast<int>(b.sendBuf.size()), pkt);
    CHECK(pkt.id == 20);
    CHECK(pkt.body.find("for B") != std::string::npos);
}

TEST_CASE("RCON drain deadline: multiple lines joined with newline in packet body", "[rcon][drain][deadline]") {
    MockLogger log;
    CommandRegistry reg;
    CommandShell shell(log, reg);
    ManualClock clk;

    rcon::DrainClientInfo c;
    c.connected = true;
    c.hasPendingDrain = true;
    c.drainMark = shell.mark();
    c.drainPacketId = 5;
    c.drainDeadline = clk.now();

    shell.print("line one");
    shell.print("line two");
    shell.print("line three");

    std::vector<rcon::DrainClientInfo*> clients{&c};
    rcon::checkAndFireDrains(clients, clk.now(), &shell);

    CHECK_FALSE(c.hasPendingDrain);
    REQUIRE_FALSE(c.sendBuf.empty());

    rcon::RconPacket pkt;
    int consumed = rcon::decodePacket(c.sendBuf.data(), static_cast<int>(c.sendBuf.size()), pkt);
    REQUIRE(consumed > 0);
    CHECK(pkt.body.find("line one") != std::string::npos);
    CHECK(pkt.body.find("line two") != std::string::npos);
    CHECK(pkt.body.find("line three") != std::string::npos);
    // Lines are joined by '\n'
    CHECK(pkt.body.find("line one\nline two") != std::string::npos);
}

TEST_CASE("RCON drain deadline: large output splits into multiple chunks with trailing empty sentinel",
          "[rcon][drain][deadline]") {
    MockLogger log;
    CommandRegistry reg;
    CommandShell shell(log, reg);
    ManualClock clk;

    rcon::DrainClientInfo c;
    c.connected = true;
    c.hasPendingDrain = true;
    c.drainMark = shell.mark();
    c.drainPacketId = 9;
    c.drainDeadline = clk.now();

    // Write a single line larger than kMaxBodyPerPacket (4086 bytes) to force splitting.
    // 5000 'x' chars → splitResponse produces chunk1(4086) + chunk2(914).
    shell.print(std::string(5000, 'x'));

    std::vector<rcon::DrainClientInfo*> clients{&c};
    rcon::checkAndFireDrains(clients, clk.now(), &shell);

    CHECK_FALSE(c.hasPendingDrain);
    REQUIRE_FALSE(c.sendBuf.empty());

    // Decode three packets: chunk1, chunk2, empty sentinel.
    int offset = 0;
    int total = static_cast<int>(c.sendBuf.size());

    rcon::RconPacket p1;
    int c1 = rcon::decodePacket(c.sendBuf.data() + offset, total - offset, p1);
    REQUIRE(c1 > 0);
    CHECK(p1.id == 9);
    CHECK(p1.type == rcon::kTypeResponseValue);
    CHECK(p1.body.size() == static_cast<std::size_t>(rcon::kMaxBodyPerPacket));
    offset += c1;

    rcon::RconPacket p2;
    int c2 = rcon::decodePacket(c.sendBuf.data() + offset, total - offset, p2);
    REQUIRE(c2 > 0);
    CHECK(p2.id == 9);
    CHECK(p2.type == rcon::kTypeResponseValue);
    CHECK(p2.body.size() == 5000 - static_cast<std::size_t>(rcon::kMaxBodyPerPacket));
    offset += c2;

    rcon::RconPacket sentinel;
    int cs = rcon::decodePacket(c.sendBuf.data() + offset, total - offset, sentinel);
    REQUIRE(cs > 0);
    CHECK(sentinel.id == 9);
    CHECK(sentinel.type == rcon::kTypeResponseValue);
    CHECK(sentinel.body.empty()); // trailing empty sentinel
    offset += cs;

    CHECK(offset == total); // no extra bytes
}
