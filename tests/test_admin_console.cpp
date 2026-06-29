// SPDX-License-Identifier: GPL-3.0-or-later
#include "ServerCommands.h"
#include <console/CommandRegistry.h>
#include <console/CommandShell.h>
#include <loop/GameLoop.h>
#include <loop/ISimUpdate.h>
#include <loop/TimeRate.h>

#include "INetwork.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "mock_network.h"
#include "net/GameProtocol.h"
#include "net/WorldBroadcaster.h"
#include <ILogger.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <csignal>
#include <cstring>
#include <string>
#include <weather/WeatherController.h>

using namespace fl;

// Fixtures used by the async-ack tests (need a real GameLoop so enqueueSimCallback is safe).
// "2" suffix avoids name collisions with any mock in mock_hal.h.
struct NullLogger2 : public ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};
struct NoopSim2 : public ISimUpdate {
    void onTick(double, uint64_t) override {}
};

// Build a registry with an all-null context (except the fields being exercised).
static CommandRegistry makeRegistry(ServerCommandContext ctx = {}) {
    CommandRegistry reg;
    registerServerCommands(reg, ctx);
    return reg;
}

// ---------------------------------------------------------------------------
// help
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: help lists registered commands", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("help");
    CHECK(out.find("status") != std::string::npos);
    CHECK(out.find("peers") != std::string::npos);
    CHECK(out.find("kick") != std::string::npos);
    CHECK(out.find("ban") != std::string::npos);
    CHECK(out.find("quit") != std::string::npos);
    CHECK(out.find("reload_banlist") != std::string::npos);
    CHECK(out.find("reload_allowlist") != std::string::npos);
}

TEST_CASE("AdminConsole: unknown command returns error", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("xyzzy");
    CHECK(out.find("unknown command") != std::string::npos);
}

// ---------------------------------------------------------------------------
// status — null context
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: status with null broadcaster returns error", "[admin_console]") {
    auto reg = makeRegistry(); // broadcaster == nullptr
    std::string out = reg.dispatch("status");
    CHECK(out.find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// kick — argument parsing
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: kick with no args returns usage", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("kick");
    CHECK(out.find("usage") != std::string::npos);
}

TEST_CASE("AdminConsole: kick with non-numeric arg and null broadcaster returns error", "[admin_console]") {
    auto reg = makeRegistry(); // broadcaster == nullptr
    std::string out = reg.dispatch("kick 1.2.3.4");
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: kick with numeric arg and null broadcaster returns error", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("kick 42");
    CHECK(out.find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// ban / unban — argument parsing
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: ban with no args returns usage", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("ban");
    CHECK(out.find("usage") != std::string::npos);
}

TEST_CASE("AdminConsole: ban with null broadcaster returns error", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("ban 1.2.3.4");
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: unban with no args returns usage", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("unban");
    CHECK(out.find("usage") != std::string::npos);
}

TEST_CASE("AdminConsole: unban with null broadcaster returns error", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("unban 1.2.3.4");
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: admin_unlock with no args returns usage", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("admin_unlock");
    CHECK(out.find("usage") != std::string::npos);
}

TEST_CASE("AdminConsole: admin_unlock with null broadcaster returns not available", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("admin_unlock 1.2.3.4");
    CHECK(out.find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// set_time — range validation
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: set_time out-of-range returns error", "[admin_console]") {
    auto reg = makeRegistry();
    CHECK(reg.dispatch("set_time 25").find("must be in") != std::string::npos);
    CHECK(reg.dispatch("set_time -1").find("must be in") != std::string::npos);
}

TEST_CASE("AdminConsole: set_time with no args returns usage", "[admin_console]") {
    auto reg = makeRegistry();
    CHECK(reg.dispatch("set_time").find("usage") != std::string::npos);
}

// ---------------------------------------------------------------------------
// reload_config — null context
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: reload_config with null configPath returns error", "[admin_console]") {
    auto reg = makeRegistry(); // configPath == nullptr
    std::string out = reg.dispatch("reload_config");
    CHECK(out.find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// quit — sets quitFlag
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: quit sets quitFlag to 1", "[admin_console]") {
    volatile sig_atomic_t flag = 0;
    ServerCommandContext ctx;
    ctx.env.quitFlag = &flag;
    auto reg = makeRegistry(ctx);
    auto out = reg.dispatch("quit");
    CHECK(flag == 1);
    CHECK(!out.empty());
}

TEST_CASE("AdminConsole: quit with null quitFlag returns error", "[admin_console]") {
    auto reg = makeRegistry(); // quitFlag == nullptr
    std::string out = reg.dispatch("quit");
    CHECK(out.find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// reload_banlist
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: reload_banlist with null banlistPath returns not available", "[admin_console][security]") {
    auto reg = makeRegistry(); // banlistPath == nullptr
    std::string out = reg.dispatch("reload_banlist");
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: reload_banlist with empty banlistPath returns not available", "[admin_console][security]") {
    ServerCommandContext ctx;
    std::string emptyPath;
    ctx.bans.banlistPath = &emptyPath; // non-null but empty
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("reload_banlist");
    CHECK(out.find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// reload_allowlist
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: reload_allowlist with null allowlistPath returns not available", "[admin_console][security]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("reload_allowlist");
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: reload_allowlist with empty allowlistPath returns not available",
          "[admin_console][security]") {
    ServerCommandContext ctx;
    std::string emptyPath;
    ctx.bans.allowlistPath = &emptyPath;
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("reload_allowlist");
    CHECK(out.find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// ban / unban — null saveBanlist does not crash
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: ban with null broadcaster returns not available", "[admin_console][security]") {
    auto reg = makeRegistry(); // broadcaster == nullptr
    std::string out = reg.dispatch("ban 1.2.3.4");
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: unban with null broadcaster returns not available", "[admin_console][security]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("unban 1.2.3.4");
    CHECK(out.find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// shutdown command
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: shutdown with null broadcaster returns not available", "[admin_console][shutdown]") {
    auto reg = makeRegistry(); // broadcaster == nullptr
    std::string out = reg.dispatch("shutdown --in 30m --force");
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --in with null gameLoop returns not available", "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    // broadcaster non-null but gameLoop == nullptr — use a sentinel address
    static int sentinel;
    ctx.sim.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.sim.gameLoop = nullptr;
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --in 30m --force");
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --in with invalid duration returns error", "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    static int sentinel;
    ctx.sim.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.sim.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --in notaduration --force");
    CHECK(out.find("invalid") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --in without --force returns confirmation prompt", "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    static int sentinel;
    ctx.sim.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.sim.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    ctx.shutdown.requireConfirm = true;
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --in 30m");
    // Should prompt to re-run with --force, not schedule anything
    CHECK(out.find("--force") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --now without --force returns confirmation prompt", "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    static int sentinel;
    ctx.sim.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.sim.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    ctx.shutdown.requireConfirm = true;
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --now");
    CHECK(out.find("--force") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --cancel with null broadcaster returns not available", "[admin_console][shutdown]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("shutdown --cancel");
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --delay with null broadcaster returns not available", "[admin_console][shutdown]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("shutdown --delay 5m");
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown unknown flag returns error", "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    static int sentinel;
    ctx.sim.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.sim.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --bogus");
    CHECK(out.find("unknown flag") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --reason without value returns error", "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    static int sentinel;
    ctx.sim.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.sim.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --in 30m --force --reason");
    CHECK(out.find("requires a value") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --in with multi-word --reason preserves confirmation prompt",
          "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    static int sentinel;
    ctx.sim.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.sim.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    ctx.shutdown.requireConfirm = true;
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --in 30m --reason scheduled maintenance");
    CHECK(out.find("--force") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --reason stops consuming at next double-dash flag", "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    static int sentinel;
    ctx.sim.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.sim.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    ctx.shutdown.requireConfirm = false;
    ctx.shutdown.minDelayS = 100;
    auto reg = makeRegistry(ctx);
    // --reason consumes "maintenance" only (stops at --force); --force bypasses the confirm gate;
    // the 10s delay is below minShutdownDelayS=100 so the min-delay gate fires.
    std::string out = reg.dispatch("shutdown --in 10s --reason maintenance --force");
    CHECK(out.find("at least") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Async command ack strings
// These tests verify that commands returning sync acknowledgments work correctly.
// A real GameLoop is required because these handlers call enqueueSimCallback.
// The sim thread is never started (loop.start() not called), so sentinels passed
// as broadcaster/entityManager are never dereferenced — they are captured only in
// lambdas that are queued but never executed.
//
// NOTE: the 'peers' command is excluded from this block because getPeerCount() is
// called during dispatch (not inside the lambda), making a sentinel broadcaster
// unsafe. Full coverage requires a real WorldBroadcaster fixture.
// ---------------------------------------------------------------------------

// Shared helper that builds a context with a real GameLoop and safe sentinel pointers.
namespace {
struct AsyncAckFixture {
    NullLogger2 log;
    NoopSim2 noop;
    GameLoop loop{noop, log}; // do NOT call loop.start()
    static int bcast_sentinel;
    static int em_sentinel;
    ServerCommandContext ctx;

    AsyncAckFixture() {
        ctx.sim.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&bcast_sentinel);
        ctx.sim.entityManager = reinterpret_cast<fl::EntityManager*>(&em_sentinel);
        ctx.sim.gameLoop = &loop;
    }
};
int AsyncAckFixture::bcast_sentinel = 0;
int AsyncAckFixture::em_sentinel = 0;
} // namespace

TEST_CASE("AdminConsole async ack: kick numeric peer returns non-empty ack with id", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("kick 42");
    CHECK_FALSE(out.empty());
    CHECK(out.find("42") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: kick IP returns non-empty ack with address", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("kick 1.2.3.4");
    CHECK_FALSE(out.empty());
    CHECK(out.find("1.2.3.4") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: ban IP returns non-empty ack", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("ban 1.2.3.4");
    CHECK_FALSE(out.empty());
}

TEST_CASE("AdminConsole async ack: unban IP returns non-empty ack with address", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("unban 1.2.3.4");
    CHECK_FALSE(out.empty());
    CHECK(out.find("1.2.3.4") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: admin_unlock IP returns non-empty ack", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_unlock 1.2.3.4");
    CHECK_FALSE(out.empty());
    CHECK(out.find("1.2.3.4") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: admin_unlock with clearRconLockout set returns ack with IP",
          "[admin_console][async_ack]") {
    AsyncAckFixture f;
    f.ctx.rcon.clearRconLockout = [](const std::string&) -> bool { return false; };
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_unlock 1.2.3.4");
    CHECK_FALSE(out.empty());
    CHECK(out.find("1.2.3.4") != std::string::npos);
}

TEST_CASE("AdminConsole: admin_unlock help text mentions both channels", "[admin_console]") {
    auto reg = makeRegistry();
    std::string help = reg.helpFor("admin_unlock");
    CHECK(help.find("RCON") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: spawn returns non-empty ack with type", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("spawn builtin:debug-entity 0 100 0");
    CHECK_FALSE(out.empty());
    CHECK(out.find("builtin:debug-entity") != std::string::npos);
}

TEST_CASE("AdminConsole: spawn --ai lua returns error when loadAIScript is null", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    f.ctx.env.loadAIScript = nullptr;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("spawn builtin:debug-entity 0 100 0 --ai lua patrol");
    // When loadAIScript is null the command returns an error string immediately.
    CHECK_FALSE(out.empty());
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: spawn --ai lua returns error when script not found", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    f.ctx.env.loadAIScript = [](std::string_view) -> std::pair<std::string, std::string> {
        return {}; // empty = not found
    };
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("spawn builtin:debug-entity 0 100 0 --ai lua patrol");
    CHECK_FALSE(out.empty());
    CHECK(out.find("not found") != std::string::npos);
}

TEST_CASE("AdminConsole: spawn --ai lua returns non-empty ack when script valid", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    f.ctx.env.loadAIScript = [](std::string_view) -> std::pair<std::string, std::string> {
        return {"function compute_control(s,t,dt) return {} end", ""};
    };
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("spawn builtin:debug-entity 0 100 0 --ai lua patrol");
    CHECK_FALSE(out.empty());
    CHECK(out.find("builtin:debug-entity") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: kill returns non-empty ack with index", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("kill 1");
    CHECK_FALSE(out.empty());
    CHECK(out.find("1") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: tp returns non-empty ack with entity index", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("tp 1 0 100 0");
    CHECK_FALSE(out.empty());
    CHECK(out.find("1") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: shutdown no-args returns status queued string", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("shutdown");
    CHECK(out == "shutdown: status queued");
}

TEST_CASE("AdminConsole async ack: shutdown --delay returns extension queued string", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    f.ctx.shutdown.requireConfirm = false;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("shutdown --delay 5m");
    CHECK(out == "shutdown: extension queued");
}

// ---------------------------------------------------------------------------
// CommandShell integration — sync ack appears in outputLines()
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole shell output: sync ack appears in outputLines", "[admin_console][shell]") {
    NullLogger2 logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);

    ServerCommandContext ctx{};
    ctx.rcon.shell = &shell;
    registerServerCommands(reg, ctx);

    // status returns a synchronous ack string; verify it also lands in the ring
    (void)shell.execute("status");

    auto lines = shell.outputLines();
    REQUIRE(!lines.empty());
    // The ring should contain at least the echo "> status" and the ack text
    bool foundEcho = false;
    for (const auto& l : lines)
        if (l.find("status") != std::string::npos)
            foundEcho = true;
    CHECK(foundEcho);
}

TEST_CASE("AdminConsole shell drain: drainSince captures post-dispatch shell output", "[admin_console][shell][drain]") {
    NullLogger2 logger;
    CommandRegistry reg;
    CommandShell shell(logger, reg);

    ServerCommandContext ctx{};
    ctx.rcon.shell = &shell;
    registerServerCommands(reg, ctx);

    // Simulate RCON thread: dispatch then snapshot mark (after dispatch to skip sync writes)
    (void)reg.dispatch("kick 42");
    int m = shell.mark();

    // Simulate sim-thread callback writing the async confirmation
    shell.print("[admin] kicked peer 42");

    // RconServer calls drainSince(mark) to get lines for RESPONSE_VALUE packets
    auto lines = shell.drainSince(m);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("kicked") != std::string::npos);
    CHECK(lines[0].find("42") != std::string::npos);
}

// ---------------------------------------------------------------------------
// pause / resume commands
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: pause sets GameLoop rate to Paused", "[admin_console][pause]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("pause");
    CHECK_FALSE(out.empty());
    CHECK(f.loop.rate() == TimeRate::Paused);
}

TEST_CASE("AdminConsole: resume sets GameLoop rate to Normal", "[admin_console][pause]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    (void)reg.dispatch("pause");
    CHECK(f.loop.rate() == TimeRate::Paused);
    std::string out = reg.dispatch("resume");
    CHECK_FALSE(out.empty());
    CHECK(f.loop.rate() == TimeRate::Normal);
}

TEST_CASE("AdminConsole: pause with null gameLoop returns error message", "[admin_console][pause]") {
    ServerCommandContext ctx{};
    ctx.sim.gameLoop = nullptr;
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("pause");
    CHECK_FALSE(out.empty()); // should return "not available" or similar, not crash
}

TEST_CASE("AdminConsole: resume with null gameLoop returns error message", "[admin_console][pause]") {
    ServerCommandContext ctx{};
    ctx.sim.gameLoop = nullptr;
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("resume");
    CHECK_FALSE(out.empty());
}

// ---------------------------------------------------------------------------
// WorldBroadcaster integration -- peers command ack
// (getPeerCount() is called synchronously during dispatch; sentinel pointers
//  from AsyncAckFixture are unsafe here -- a real WorldBroadcaster is required)
// ---------------------------------------------------------------------------

namespace {
// Null network with a single configurable peer address for all peers (see mock_network.h).
struct MockNetworkWb : NullNetwork {
    std::string peerAddr; // set to e.g. "1.2.3.4" to test IP-based paths
    const char* getPeerAddress(uint32_t) const override {
        return peerAddr.empty() ? nullptr : peerAddr.c_str();
    }
};

static fl::EntityDef makeWbEntityDef(const char* id = "builtin:debug-entity") {
    fl::EntityDef def;
    def.id = id;
    def.name = "Debug";
    def.category = fl::ObjectCategory::AirVehicle;
    def.maxHp = 100.0f;
    return def;
}

struct WbFixture {
    NullLogger2 log;
    MockNetworkWb net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em{log, registry};
    fl::WorldBroadcaster broadcaster{em, registry, net, log};
    NoopSim2 noop;
    GameLoop loop{noop, log}; // do NOT call loop.start()
    ServerCommandContext ctx;

    WbFixture() {
        ctx.sim.broadcaster = &broadcaster;
        ctx.sim.entityManager = &em;
        ctx.sim.gameLoop = &loop;
    }
};
} // namespace

TEST_CASE("AdminConsole: peers with null broadcaster returns not available", "[admin_console]") {
    auto reg = makeRegistry(); // broadcaster == nullptr
    std::string out = reg.dispatch("peers");
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole wb: peers with null gameLoop returns not available", "[admin_console][wb]") {
    WbFixture f;
    f.ctx.sim.gameLoop = nullptr;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("peers");
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole wb: peers with no connected peers returns 0 peer(s) connected", "[admin_console][wb]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("peers");
    CHECK(out == "0 peer(s) connected");
}

TEST_CASE("AdminConsole wb: peers with one connected peer returns 1 peer(s) connected", "[admin_console][wb]") {
    WbFixture f;
    f.registry.registerType(makeWbEntityDef());
    f.broadcaster.onConnect(0u);

    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("peers");
    CHECK(out == "1 peer(s) connected");
}

// ---------------------------------------------------------------------------
// WorldBroadcaster integration -- status command ack
// (getPeerCount() and liveCount() are called synchronously during dispatch;
//  sentinel pointers from AsyncAckFixture are unsafe here)
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole wb: status with null entityManager returns not available", "[admin_console][wb]") {
    WbFixture f;
    f.ctx.sim.entityManager = nullptr; // broadcaster is real; entityManager is null
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("status");
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole wb: status with zero peers contains peers: 0", "[admin_console][wb]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("status");
    CHECK(out.find("peers: 0") != std::string::npos);
}

TEST_CASE("AdminConsole wb: status with one connected peer contains peers: 1", "[admin_console][wb]") {
    WbFixture f;
    f.registry.registerType(makeWbEntityDef());
    f.broadcaster.onConnect(0u);

    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("status");
    CHECK(out.find("peers: 1") != std::string::npos);
}

TEST_CASE("AdminConsole wb: status shows the real tick Hz line", "[admin_console][wb]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("status");
    CHECK(out.find("tick:") != std::string::npos);
    CHECK(out.find("Hz") != std::string::npos);
    CHECK(out.find("mean/p99") != std::string::npos);
}

// ---------------------------------------------------------------------------
// tickstats command tests
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: tickstats with null broadcaster returns not available", "[admin_console]") {
    auto reg = makeRegistry(); // broadcaster == nullptr
    std::string out = reg.dispatch("tickstats");
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole wb: tickstats before any tick reports no samples", "[admin_console][wb]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("tickstats");
    CHECK(out.find("no ticks sampled") != std::string::npos);
}

TEST_CASE("AdminConsole wb: tickstats reports per-phase rows after ticks", "[admin_console][wb]") {
    WbFixture f;
    f.registry.registerType(makeWbEntityDef());
    f.broadcaster.onConnect(0u);
    for (uint64_t tick = 1; tick <= 5; ++tick)
        f.broadcaster.onTick(1.0 / 60.0, tick);

    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("tickstats");
    CHECK(out.find("tick") != std::string::npos);
    CHECK(out.find("integrate") != std::string::npos);
    CHECK(out.find("ai") != std::string::npos);
    CHECK(out.find("collision") != std::string::npos);
    CHECK(out.find("serialize") != std::string::npos);
    CHECK(out.find("total") != std::string::npos);
}

// ---------------------------------------------------------------------------
// admin_auth_status command tests
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: admin_auth_status with null broadcaster returns not available", "[admin_console]") {
    auto reg = makeRegistry(); // broadcaster == nullptr
    std::string out = reg.dispatch("admin_auth_status");
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole wb: admin_auth_status with no lockouts returns section header", "[admin_console][wb]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_auth_status");
    CHECK(out == "[admin] MsgAdminCommand channel:");
}

TEST_CASE("AdminConsole wb: status with no lockouts does not show lockout line", "[admin_console][wb]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("status");
    CHECK(out.find("admin auth lockouts") == std::string::npos);
}

TEST_CASE("AdminConsole wb: status and admin_auth_status reflect active lockout", "[admin_console][wb]") {
    WbFixture f;
    f.net.peerAddr = "1.2.3.4";
    f.broadcaster.setAdminAuthParams(1, 300);
    f.broadcaster.setOperatorPassword("correct");
    f.broadcaster.setAdminDispatch([](std::string_view) { return std::string{}; });
    f.registry.registerType(makeWbEntityDef());
    f.broadcaster.onConnect(0u);

    fl::MsgAdminCommand cmd{};
    std::snprintf(cmd.token, sizeof(cmd.token), "%s", "wrongpass");
    std::snprintf(cmd.command, sizeof(cmd.command), "%s", "status");
    f.broadcaster.onReceive(0u, &cmd, sizeof(cmd));

    auto reg = makeRegistry(f.ctx);

    std::string statusOut = reg.dispatch("status");
    CHECK(statusOut.find("admin auth lockouts: 1 active") != std::string::npos);
    CHECK(statusOut.find("use admin_auth_status") != std::string::npos);

    std::string authOut = reg.dispatch("admin_auth_status");
    CHECK(authOut.find("MsgAdminCommand channel:") != std::string::npos);
    CHECK(authOut.find("1.2.3.4") != std::string::npos);
    CHECK(authOut.find("locked out") != std::string::npos);
}

TEST_CASE("AdminConsole wb: admin_auth_status shows pending failure line", "[admin_console][wb]") {
    WbFixture f;
    f.net.peerAddr = "1.2.3.4";
    f.broadcaster.setAdminAuthParams(3, 300);
    f.broadcaster.setOperatorPassword("correct");
    f.broadcaster.setAdminDispatch([](std::string_view) { return std::string{}; });
    f.registry.registerType(makeWbEntityDef());
    f.broadcaster.onConnect(0u);

    fl::MsgAdminCommand cmd{};
    std::snprintf(cmd.token, sizeof(cmd.token), "%s", "wrongpass");
    std::snprintf(cmd.command, sizeof(cmd.command), "%s", "status");
    f.broadcaster.onReceive(0u, &cmd, sizeof(cmd)); // 1 failure, threshold=3, no lockout

    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_auth_status");
    CHECK(out.find("1.2.3.4") != std::string::npos);
    CHECK(out.find("1 failure(s)") != std::string::npos);
    CHECK(out.find("threshold: 3") != std::string::npos);
}

TEST_CASE("AdminConsole wb: admin_auth_status no RCON callback shows only admin section", "[admin_console][wb]") {
    WbFixture f; // ctx.rcon.getRconAuthSummary is null by default
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_auth_status");
    CHECK(out.find("MsgAdminCommand channel:") != std::string::npos);
    CHECK(out.find("RCON channel:") == std::string::npos);
}

TEST_CASE("AdminConsole wb: admin_auth_status with RCON callback shows both sections when zero entries",
          "[admin_console][wb]") {
    WbFixture f;
    f.ctx.rcon.getRconAuthSummary = []() { return fl::AuthLockoutSummary{}; };
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_auth_status");
    CHECK(out.find("MsgAdminCommand channel:") != std::string::npos);
    CHECK(out.find("RCON channel:") != std::string::npos);
}

TEST_CASE("AdminConsole wb: admin_auth_status with RCON callback shows locked-out RCON entry", "[admin_console][wb]") {
    WbFixture f;
    fl::AuthLockoutSummary rconS;
    rconS.activeCount = 1;
    rconS.threshold = 5;
    rconS.entries.push_back({"5.6.7.8", true, 0, 120LL});
    f.ctx.rcon.getRconAuthSummary = [rconS]() { return rconS; };
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_auth_status");
    CHECK(out.find("RCON channel:") != std::string::npos);
    CHECK(out.find("5.6.7.8") != std::string::npos);
    CHECK(out.find("locked out") != std::string::npos);
}

TEST_CASE("AdminConsole wb: admin_auth_status with RCON callback shows pending RCON failures", "[admin_console][wb]") {
    WbFixture f;
    fl::AuthLockoutSummary rconS;
    rconS.activeCount = 0;
    rconS.threshold = 5;
    rconS.entries.push_back({"9.10.11.12", false, 3, 0LL});
    f.ctx.rcon.getRconAuthSummary = [rconS]() { return rconS; };
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_auth_status");
    CHECK(out.find("RCON channel:") != std::string::npos);
    CHECK(out.find("9.10.11.12") != std::string::npos);
    CHECK(out.find("3 failure(s)") != std::string::npos);
    CHECK(out.find("threshold: 5") != std::string::npos);
}

TEST_CASE("AdminConsole wb: status shows no lockout line after admin_unlock clears it", "[admin_console][wb]") {
    WbFixture f;
    f.net.peerAddr = "1.2.3.4";
    f.broadcaster.setAdminAuthParams(1, 300);
    f.broadcaster.setOperatorPassword("correct");
    f.broadcaster.setAdminDispatch([](std::string_view) { return std::string{}; });
    f.registry.registerType(makeWbEntityDef());
    f.broadcaster.onConnect(0u);

    fl::MsgAdminCommand cmd{};
    std::snprintf(cmd.token, sizeof(cmd.token), "%s", "wrongpass");
    std::snprintf(cmd.command, sizeof(cmd.command), "%s", "status");
    f.broadcaster.onReceive(0u, &cmd, sizeof(cmd)); // triggers lockout

    f.broadcaster.unlockAdminAuth("1.2.3.4");

    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("status");
    CHECK(out.find("admin auth lockouts") == std::string::npos);
}

TEST_CASE("WorldBroadcaster: default MsgConnectAck carries Earth planet radius", "[admin_console][wb]") {
    NullLogger2 log;
    TrackingNetwork net;
    net.peerAddresses[0u] = "1.2.3.4";
    fl::EntityTypeRegistry registry;
    fl::EntityManager em{log, registry};
    fl::WorldBroadcaster broadcaster{em, registry, net, log};
    registry.registerType(makeWbEntityDef());

    broadcaster.onConnect(0u);

    bool found = false;
    for (const auto& pkt : net.sends) {
        if (pkt.size() >= sizeof(fl::MsgConnectAck) && pkt[0] == static_cast<uint8_t>(fl::MsgId::ConnectAck)) {
            fl::MsgConnectAck ack{};
            std::memcpy(&ack, pkt.data(), sizeof(ack));
            CHECK(ack.planetRadiusKm == Catch::Approx(6371.f).epsilon(0.001f));
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("WorldBroadcaster: setGroundElevationQuery is called per entity during onTick", "[admin_console][wb]") {
    WbFixture f;
    f.registry.registerType(makeWbEntityDef());

    int queryCalls = 0;
    f.broadcaster.setGroundElevationQuery([&](double, double) {
        ++queryCalls;
        return 42.f;
    });
    f.broadcaster.onConnect(0u);
    f.broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK(queryCalls > 0);
}

TEST_CASE("AdminConsole: set_weather snow enqueues preset change", "[admin_console]") {
    AsyncAckFixture f;
    fl::WeatherController wc;
    f.ctx.sim.weatherController = &wc;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("set_weather snow");
    CHECK(out.find("snow") != std::string::npos);
    CHECK(out.find("not available") == std::string::npos);
}

TEST_CASE("AdminConsole: set_weather blizzard enqueues preset change", "[admin_console]") {
    AsyncAckFixture f;
    fl::WeatherController wc;
    f.ctx.sim.weatherController = &wc;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("set_weather blizzard");
    CHECK(out.find("blizzard") != std::string::npos);
    CHECK(out.find("not available") == std::string::npos);
}
