// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for AdminChannel + AdminChannelRegistry (#1079, D14).
//
// What these pin is the thing the six forked frontends kept getting differently: per-channel lockout
// isolation, an operator being able to see and clear EVERY channel without naming one, an absent
// credential not counting as a brute-force attempt, and one drain implementation with an injected clock
// instead of two hand-rolled ones that independently picked the same 20 ms.
#include "IClock.h"
#include "net/AdminChannel.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace fl;

namespace {

// A dispatcher that records what it was asked and echoes the issuer's authority, so a test can see that
// the capability-checked path is the one being taken.
struct RecordingDispatcher {
    std::vector<std::pair<std::string, CapabilityMask>> calls;

    AdminChannel::Dispatcher fn() {
        return [this](std::string_view line, const CommandIssuer& issuer) {
            calls.emplace_back(std::string(line), issuer.caps);
            return "ok: " + std::string(line);
        };
    }
};

AdminChannel::Config cfg(std::string name, int maxFailures = 3, int lockoutSeconds = 300) {
    AdminChannel::Config c;
    c.name = std::move(name);
    c.maxAuthFailures = maxFailures;
    c.lockoutSeconds = lockoutSeconds;
    return c;
}

} // namespace

TEST_CASE("AdminChannel: dispatch carries the issuer through to the registry", "[admin_channel]") {
    ManualClock clock;
    RecordingDispatcher d;
    AdminChannel channel(d.fn(), cfg("rcon"), clock);

    const CommandIssuer limited{7, kModeratorCaps, 2};
    CHECK(channel.dispatch("status", limited) == "ok: status");
    REQUIRE(d.calls.size() == 1);
    CHECK(d.calls[0].first == "status");
    CHECK(d.calls[0].second == kModeratorCaps); // not silently promoted to kAdminCaps
}

TEST_CASE("AdminChannel: a channel with no dispatcher says so rather than returning nothing", "[admin_channel]") {
    ManualClock clock;
    AdminChannel channel(nullptr, cfg("mission"), clock);
    const std::string out = channel.dispatch("kick 1", CommandIssuer{});
    CHECK(out.find("mission") != std::string::npos);
    CHECK(!out.empty()); // a mis-wired frontend is visible, not silent
}

TEST_CASE("AdminChannel: lockout trips at the threshold and clears on success", "[admin_channel]") {
    ManualClock clock;
    RecordingDispatcher d;
    AdminChannel channel(d.fn(), cfg("enet", /*maxFailures=*/3), clock);

    CHECK_FALSE(channel.lockedOut("10.0.0.1"));
    CHECK_FALSE(channel.recordAuthResult("10.0.0.1", false));
    CHECK_FALSE(channel.recordAuthResult("10.0.0.1", false));
    CHECK(channel.recordAuthResult("10.0.0.1", false)); // the third trips it
    CHECK(channel.lockedOut("10.0.0.1"));

    // A success clears the counter but not an active lockout -- the pre-existing AuthTracker contract.
    CHECK(channel.clearLockout("10.0.0.1"));
    CHECK_FALSE(channel.lockedOut("10.0.0.1"));
    CHECK_FALSE(channel.clearLockout("10.0.0.1")); // idempotent, and reports that nothing was locked
}

TEST_CASE("AdminChannel: an absent credential is a refusal, not a brute-force attempt", "[admin_channel]") {
    // #947: a peer with no granted caps must not be able to lock out the operator sharing its NAT
    // address. `attempted = false` says "presented nothing", which is a permission refusal.
    ManualClock clock;
    RecordingDispatcher d;
    AdminChannel channel(d.fn(), cfg("enet", /*maxFailures=*/2), clock);

    for (int i = 0; i < 10; ++i)
        CHECK_FALSE(channel.recordAuthResult("10.0.0.2", false, /*attempted=*/false));
    CHECK_FALSE(channel.lockedOut("10.0.0.2"));

    // A real wrong credential from the same address still counts.
    CHECK_FALSE(channel.recordAuthResult("10.0.0.2", false));
    CHECK(channel.recordAuthResult("10.0.0.2", false));
    CHECK(channel.lockedOut("10.0.0.2"));
}

TEST_CASE("AdminChannel: lockouts are per channel, so one frontend cannot lock out another", "[admin_channel]") {
    ManualClock clock;
    RecordingDispatcher d;
    AdminChannel rcon(d.fn(), cfg("rcon", 1), clock);
    AdminChannel http(d.fn(), cfg("http", 1), clock);

    CHECK(rcon.recordAuthResult("10.0.0.3", false));
    CHECK(rcon.lockedOut("10.0.0.3"));
    CHECK_FALSE(http.lockedOut("10.0.0.3")); // isolation: a bad RCON password is not a bad bearer token
}

TEST_CASE("AdminChannelRegistry: an operator clears every channel without naming one", "[admin_channel]") {
    // The defect this replaces: admin_unlock named "admin + RCON + HTTP" by hand, and its own comment
    // said that unlocking two of three and leaving the third would be worse than not unlocking at all.
    // A fourth channel was silently missed.
    ManualClock clock;
    RecordingDispatcher d;
    AdminChannel enet(d.fn(), cfg("enet", 1), clock);
    AdminChannel rcon(d.fn(), cfg("rcon", 1), clock);
    AdminChannel http(d.fn(), cfg("http", 1), clock);
    AdminChannel mcp(d.fn(), cfg("mcp", 1), clock); // the "fourth channel" case

    AdminChannelRegistry registry;
    registry.add(enet);
    registry.add(rcon);
    registry.add(http);
    registry.add(mcp);
    CHECK(registry.size() == 4);

    CHECK(enet.recordAuthResult("10.0.0.4", false));
    CHECK(mcp.recordAuthResult("10.0.0.4", false));
    CHECK(registry.activeLockoutCount() == 2);

    const auto cleared = registry.clearLockoutEverywhere("10.0.0.4");
    // Reports what actually changed, rather than a fixed list of channel names.
    CHECK(cleared == std::vector<std::string>{"enet", "mcp"});
    CHECK(registry.activeLockoutCount() == 0);
    CHECK_FALSE(enet.lockedOut("10.0.0.4"));
    CHECK_FALSE(mcp.lockedOut("10.0.0.4"));
}

TEST_CASE("AdminChannelRegistry: summaries are per channel, in registration order", "[admin_channel]") {
    ManualClock clock;
    RecordingDispatcher d;
    AdminChannel a(d.fn(), cfg("stdin", 5), clock);
    AdminChannel b(d.fn(), cfg("rcon", 3), clock);
    AdminChannelRegistry registry;
    registry.add(a);
    registry.add(b);

    const auto s = registry.summaries();
    REQUIRE(s.size() == 2);
    CHECK(s[0].first == "stdin");
    CHECK(s[0].second.threshold == 5);
    CHECK(s[1].first == "rcon");
    CHECK(s[1].second.threshold == 3); // each channel reports ITS threshold, not a shared one
}

// ---------------------------------------------------------------------------
// The async-ack drain — one implementation, injected clock
// ---------------------------------------------------------------------------

TEST_CASE("AdminChannel: a drain fires once its deadline passes, with the lines written after dispatch",
          "[admin_channel][drain]") {
    ManualClock clock;
    RecordingDispatcher d;
    AdminChannel channel(d.fn(), cfg("rcon"), clock);

    std::vector<std::string> shell;
    channel.setShellTap([&shell] { return static_cast<int>(shell.size()); },
                        [&shell](int mark) { return std::vector<std::string>(shell.begin() + mark, shell.end()); });

    shell.push_back("output from an earlier command");
    channel.armDrain(/*token=*/42);
    CHECK(channel.pendingDrainCount() == 1);

    // The mutating command's output lands after dispatch returned, via enqueueSimCallback.
    shell.push_back("[admin] kicked peer 3");

    std::vector<std::pair<uint64_t, std::vector<std::string>>> emitted;
    const auto emit = [&emitted](uint64_t token, const std::vector<std::string>& lines) {
        emitted.emplace_back(token, lines);
    };

    channel.serviceDrains(emit);
    CHECK(emitted.empty()); // deadline not reached
    CHECK(channel.pendingDrainCount() == 1);

    clock.advance(std::chrono::milliseconds(25));
    channel.serviceDrains(emit);
    REQUIRE(emitted.size() == 1);
    CHECK(emitted[0].first == 42);
    // Only what appeared AFTER the mark: the earlier line is not re-delivered.
    CHECK(emitted[0].second == std::vector<std::string>{"[admin] kicked peer 3"});
    CHECK(channel.pendingDrainCount() == 0);

    channel.serviceDrains(emit);
    CHECK(emitted.size() == 1); // fires exactly once
}

TEST_CASE("AdminChannel: a drain that produced no output emits nothing", "[admin_channel][drain]") {
    ManualClock clock;
    RecordingDispatcher d;
    AdminChannel channel(d.fn(), cfg("enet"), clock);
    std::vector<std::string> shell;
    channel.setShellTap([&shell] { return static_cast<int>(shell.size()); },
                        [&shell](int mark) { return std::vector<std::string>(shell.begin() + mark, shell.end()); });

    channel.armDrain(1);
    clock.advance(std::chrono::milliseconds(25));
    int emitCount = 0;
    channel.serviceDrains([&emitCount](uint64_t, const std::vector<std::string>&) { ++emitCount; });
    CHECK(emitCount == 0); // a read-only command needs no follow-on packet
    CHECK(channel.pendingDrainCount() == 0);
}

TEST_CASE("AdminChannel: cancelling a drain drops it, and re-arming a token replaces it", "[admin_channel][drain]") {
    ManualClock clock;
    RecordingDispatcher d;
    AdminChannel channel(d.fn(), cfg("rcon"), clock);
    std::vector<std::string> shell;
    channel.setShellTap([&shell] { return static_cast<int>(shell.size()); },
                        [&shell](int mark) { return std::vector<std::string>(shell.begin() + mark, shell.end()); });

    channel.armDrain(1);
    channel.armDrain(1); // a client that disconnects and reconnects reuses its packet id
    CHECK(channel.pendingDrainCount() == 1);

    channel.cancelDrain(1); // the transport dropped the client
    CHECK(channel.pendingDrainCount() == 0);
    clock.advance(std::chrono::milliseconds(25));
    int emitCount = 0;
    channel.serviceDrains([&emitCount](uint64_t, const std::vector<std::string>&) { ++emitCount; });
    CHECK(emitCount == 0); // nothing is written to a socket that is gone
}

TEST_CASE("AdminChannel: a channel with no shell tap simply has no drain", "[admin_channel][drain]") {
    // HTTP and MCP: they hold no shell, and arming is a no-op rather than a queue that never fires.
    ManualClock clock;
    RecordingDispatcher d;
    AdminChannel channel(d.fn(), cfg("http"), clock);
    channel.armDrain(1);
    CHECK(channel.pendingDrainCount() == 0);
    clock.advance(std::chrono::milliseconds(100));
    int emitCount = 0;
    channel.serviceDrains([&emitCount](uint64_t, const std::vector<std::string>&) { ++emitCount; });
    CHECK(emitCount == 0);
}
