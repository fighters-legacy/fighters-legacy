// SPDX-License-Identifier: GPL-3.0-or-later
#include "IClock.h"
#include "RconServer.h"
#include "console/CommandRegistry.h"
#include "console/CommandShell.h"
#include "mock_hal.h"
#include "net/AdminChannel.h"
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

TEST_CASE("RconServer: its lockout is the channel's, readable without a socket", "[rcon]") {
    // The server no longer owns an AuthTracker or a mutex around one (#1079): admin_unlock and
    // admin_auth_status reach the same state through the registry, so there is nothing to forward.
    MockLogger log;
    ManualClock clk;
    AdminChannel::Config cc;
    cc.name = "rcon";
    cc.maxAuthFailures = 3;
    cc.lockoutSeconds = 120;
    AdminChannel channel([](std::string_view, const CommandIssuer&) { return std::string{}; }, cc, clk);
    ServerConfig::RconConfig cfg{};
    RconServer srv(channel, cfg, log); // start() not called — no sockets opened

    CHECK_FALSE(channel.clearLockout("1.2.3.4"));
    auto s = channel.authSummary();
    CHECK(s.activeCount == 0);
    CHECK(s.threshold == 3);
    CHECK(s.entries.empty());
}

TEST_CASE("RconServer: setClock can be called before start and does not crash", "[rcon]") {
    MockLogger log;
    ManualClock clk;
    AdminChannel channel([](std::string_view, const CommandIssuer&) { return std::string{}; },
                         AdminChannel::Config{"rcon"}, clk);
    ServerConfig::RconConfig cfg{};
    RconServer srv(channel, cfg, log);
    srv.setClock(clk); // pimpl forwarding; no start() called, no sockets
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
// Drain encoding + deadline
//
// The DEADLINE is AdminChannel's now (#1079) -- both frontends hand-rolled the same 20 ms wall-clock
// rule, so it is tested once, against the channel. What stays RCON's is the ENCODING: joining the
// drained lines, splitting at kMaxBodyPerPacket, and the empty sentinel that tells a Source client the
// multi-packet response ended. These tests drive the two together the way ioLoop does.
// ---------------------------------------------------------------------------

namespace {

// The channel an RCON client's drain is armed on, wired to a real CommandShell exactly as fl-server
// wires it. `kToken` stands in for the per-client id ioLoop assigns on accept.
constexpr uint64_t kToken = 1;

struct DrainFixture {
    MockLogger log;
    CommandRegistry reg;
    CommandShell shell{log, reg};
    ManualClock clk;
    AdminChannel channel{[](std::string_view, const CommandIssuer&) { return std::string{}; },
                         AdminChannel::Config{"rcon"}, clk};
    std::vector<uint8_t> sendBuf;

    DrainFixture() {
        channel.setShellTap([this]() { return shell.mark(); }, [this](int m) { return shell.drainSince(m); });
    }

    // One service pass, encoding whatever fired into sendBuf under `packetId`.
    void service(int32_t packetId) {
        channel.serviceDrains([&](uint64_t, const std::vector<std::string>& lines) {
            auto bytes = rcon::encodeDrainPackets(packetId, lines);
            sendBuf.insert(sendBuf.end(), bytes.begin(), bytes.end());
        });
    }
};

} // namespace

TEST_CASE("RCON drain: does not fire before the channel's deadline", "[rcon][drain][deadline]") {
    DrainFixture f;
    f.channel.armDrain(kToken);
    f.shell.print("async output");

    f.service(1);

    CHECK(f.channel.pendingDrainCount() == 1);
    CHECK(f.sendBuf.empty());
}

TEST_CASE("RCON drain: fires once the deadline passes, single line", "[rcon][drain][deadline]") {
    DrainFixture f;
    f.channel.armDrain(kToken);
    f.shell.print("kick confirmed");
    f.clk.advance(std::chrono::milliseconds(20));

    f.service(7);

    CHECK(f.channel.pendingDrainCount() == 0);
    REQUIRE_FALSE(f.sendBuf.empty());
    rcon::RconPacket pkt;
    const int consumed = rcon::decodePacket(f.sendBuf.data(), static_cast<int>(f.sendBuf.size()), pkt);
    REQUIRE(consumed > 0);
    CHECK(pkt.id == 7);
    CHECK(pkt.type == rcon::kTypeResponseValue);
    CHECK(pkt.body.find("kick confirmed") != std::string::npos);
}

TEST_CASE("RCON drain: fires exactly once, not retriggered on the next service pass", "[rcon][drain][deadline]") {
    DrainFixture f;
    f.channel.armDrain(kToken);
    f.shell.print("first output");
    f.clk.advance(std::chrono::milliseconds(20));

    f.service(1);
    const std::size_t afterFirst = f.sendBuf.size();
    REQUIRE(afterFirst > 0);

    f.shell.print("second output");
    f.service(1);

    CHECK(f.sendBuf.size() == afterFirst);
}

TEST_CASE("RCON drain: queues no packets when the shell wrote nothing since the mark", "[rcon][drain][deadline]") {
    DrainFixture f;
    f.shell.print("before mark"); // written before arming — not visible to drainSince
    f.channel.armDrain(kToken);
    f.clk.advance(std::chrono::milliseconds(20));

    f.service(3);

    CHECK(f.channel.pendingDrainCount() == 0); // cleared even with no output
    CHECK(f.sendBuf.empty());
}

TEST_CASE("RCON drain: a client that disconnected before the deadline is not written to", "[rcon][drain][deadline]") {
    // ioLoop cancels by token when it reaps a closed socket; nothing reaches a send buffer that is
    // about to be destroyed.
    DrainFixture f;
    f.channel.armDrain(kToken);
    f.shell.print("output");
    f.channel.cancelDrainsWhere([](uint64_t t) { return t == kToken; });
    f.clk.advance(std::chrono::milliseconds(20));

    f.service(1);

    CHECK(f.channel.pendingDrainCount() == 0);
    CHECK(f.sendBuf.empty());
}

TEST_CASE("RCON drain: with two clients armed, only the expired one fires", "[rcon][drain][deadline]") {
    DrainFixture f;
    f.channel.armDrain(10); // client A, armed now
    f.clk.advance(std::chrono::milliseconds(20));
    f.channel.armDrain(20); // client B, armed 20 ms later — deadline still ahead
    f.shell.print("for A");

    std::vector<uint64_t> fired;
    f.channel.serviceDrains([&](uint64_t token, const std::vector<std::string>&) { fired.push_back(token); });

    REQUIRE(fired.size() == 1);
    CHECK(fired[0] == 10);
    CHECK(f.channel.pendingDrainCount() == 1); // B still waiting
}

TEST_CASE("RCON drain: multiple lines are joined with newlines in one packet body", "[rcon][drain][deadline]") {
    DrainFixture f;
    f.channel.armDrain(kToken);
    f.shell.print("line one");
    f.shell.print("line two");
    f.shell.print("line three");
    f.clk.advance(std::chrono::milliseconds(20));

    f.service(5);

    REQUIRE_FALSE(f.sendBuf.empty());
    rcon::RconPacket pkt;
    const int consumed = rcon::decodePacket(f.sendBuf.data(), static_cast<int>(f.sendBuf.size()), pkt);
    REQUIRE(consumed > 0);
    CHECK(pkt.body.find("line one\nline two") != std::string::npos);
    CHECK(pkt.body.find("line three") != std::string::npos);
}

TEST_CASE("RCON drain: large output splits into chunks with a trailing empty sentinel", "[rcon][drain][deadline]") {
    DrainFixture f;
    f.channel.armDrain(kToken);
    // A single line larger than kMaxBodyPerPacket (4086) forces the split: 5000 'x' →
    // chunk1(4086) + chunk2(914) + sentinel.
    f.shell.print(std::string(5000, 'x'));
    f.clk.advance(std::chrono::milliseconds(20));

    f.service(9);

    REQUIRE_FALSE(f.sendBuf.empty());
    int offset = 0;
    const int total = static_cast<int>(f.sendBuf.size());

    rcon::RconPacket p1;
    const int c1 = rcon::decodePacket(f.sendBuf.data() + offset, total - offset, p1);
    REQUIRE(c1 > 0);
    CHECK(p1.id == 9);
    CHECK(p1.body.size() == static_cast<std::size_t>(rcon::kMaxBodyPerPacket));
    offset += c1;

    rcon::RconPacket p2;
    const int c2 = rcon::decodePacket(f.sendBuf.data() + offset, total - offset, p2);
    REQUIRE(c2 > 0);
    CHECK(p2.id == 9);
    CHECK(p2.body.size() == 5000 - static_cast<std::size_t>(rcon::kMaxBodyPerPacket));
    offset += c2;

    rcon::RconPacket sentinel;
    const int cs = rcon::decodePacket(f.sendBuf.data() + offset, total - offset, sentinel);
    REQUIRE(cs > 0);
    CHECK(sentinel.id == 9);
    CHECK(sentinel.body.empty()); // trailing empty sentinel
    offset += cs;

    CHECK(offset == total); // no extra bytes
}

TEST_CASE("RCON drain: no lines encodes to no bytes at all", "[rcon][drain]") {
    CHECK(rcon::encodeDrainPackets(1, {}).empty());
}
