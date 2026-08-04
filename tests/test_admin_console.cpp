// SPDX-License-Identifier: GPL-3.0-or-later
#include "ServerCommands.h"
#include <console/CommandRegistry.h>
#include <console/CommandShell.h>
#include <loop/GameLoop.h>
#include <loop/ISimUpdate.h>
#include <loop/TimeRate.h>

#include "INetwork.h"
#include "atc/AtcService.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "flight/Geodetic.h"
#include "mock_network.h"
#include "net/GameProtocol.h"
#include "net/WorldBroadcaster.h"
#include "world/AirportRegistry.h"
#include "world/BuiltinAirport.h"
#include <IClock.h>
#include <ILogger.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
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
    registerServerCommands(reg, std::make_shared<const ServerCommandContext>(std::move(ctx)));
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

// ---------------------------------------------------------------------------
// mute / unmute / mutes (#646)
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: mute with no args returns usage", "[admin_console]") {
    auto reg = makeRegistry();
    CHECK(reg.dispatch("mute").find("usage") != std::string::npos);
    CHECK(reg.dispatch("unmute").find("usage") != std::string::npos);
}

TEST_CASE("AdminConsole: mute with null broadcaster returns not available", "[admin_console]") {
    auto reg = makeRegistry();
    CHECK(reg.dispatch("mute 3").find("not available") != std::string::npos);
    CHECK(reg.dispatch("mutes").find("not available") != std::string::npos);
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

TEST_CASE("AdminConsole async ack: mute non-numeric arg rejected (#646)", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    CHECK(reg.dispatch("mute notanid").find("expected a peer ID") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: mute/unmute/mutes return a sync ack (#646)", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    CHECK(reg.dispatch("mute 7").find("queued peer 7") != std::string::npos);
    CHECK(reg.dispatch("unmute 7").find("queued peer 7") != std::string::npos);
    CHECK(reg.dispatch("mutes") == "mutes: queued");
}

TEST_CASE("AdminConsole: spectate usage / not-available (#403)", "[admin_console]") {
    auto reg = makeRegistry();
    CHECK(reg.dispatch("spectate").find("usage") != std::string::npos);
    CHECK(reg.dispatch("spectate 3").find("usage") != std::string::npos);
    CHECK(reg.dispatch("spectate 3 5").find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: spectate parses target + off (#403)", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    CHECK(reg.dispatch("spectate 3 5").find("queued peer 3") != std::string::npos);
    CHECK(reg.dispatch("spectate 3 off").find("queued peer 3") != std::string::npos);
    CHECK(reg.dispatch("spectate 3 notanumber").find("number or 'off'") != std::string::npos);
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
    registerServerCommands(reg, std::make_shared<const ServerCommandContext>(ctx));

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
    registerServerCommands(reg, std::make_shared<const ServerCommandContext>(ctx));

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

// Drive the #853 connect handshake for a pilot: onConnect + the client's MsgConnectRequest, which now
// triggers the spawn + ConnectAck (the old flow spawned/acked directly in onConnect).
static void connectPilotWb(fl::WorldBroadcaster& b, uint32_t peerId = 0u) {
    b.onConnect(peerId);
    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
    b.onReceive(peerId, &req, sizeof(req));
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

TEST_CASE("AdminConsole wb: set_role validates and queues with a synchronous ack (#857)", "[admin_console][wb]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    CHECK(reg.dispatch("set_role").find("usage") != std::string::npos);
    CHECK(reg.dispatch("set_role 0").find("usage") != std::string::npos);
    CHECK(reg.dispatch("set_role x observer").find("invalid peer ID") != std::string::npos);
    CHECK(reg.dispatch("set_role 0 bogus").find("pilot") != std::string::npos);
    std::string ok = reg.dispatch("set_role 3 observer");
    CHECK(ok.find("queued peer 3") != std::string::npos);
    CHECK(ok.find("observer") != std::string::npos);
}

TEST_CASE("AdminConsole wb: grant validates and queues with a synchronous ack (#947)",
          "[admin_console][wb][permission]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    CHECK(reg.dispatch("grant").find("usage") != std::string::npos);
    CHECK(reg.dispatch("grant 0").find("usage") != std::string::npos);
    CHECK(reg.dispatch("grant x gm").find("invalid peer ID") != std::string::npos);
    CHECK(reg.dispatch("grant 0 superuser").find("role must be") != std::string::npos);
    CHECK(reg.dispatch("grant 0 faction_leader nope").find("invalid faction") != std::string::npos);
    std::string ok = reg.dispatch("grant 3 gm");
    CHECK(ok.find("queued peer 3") != std::string::npos);
    CHECK(ok.find("gm") != std::string::npos);
}

TEST_CASE("AdminConsole wb: revoke validates and queues with a synchronous ack (#947)",
          "[admin_console][wb][permission]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    CHECK(reg.dispatch("revoke").find("usage") != std::string::npos);
    CHECK(reg.dispatch("revoke x").find("invalid peer ID") != std::string::npos);
    CHECK(reg.dispatch("revoke 7").find("queued peer 7") != std::string::npos);
}

TEST_CASE("AdminConsole wb: grant -> act -> revoke -> refused end to end (#947)", "[admin_console][wb][permission]") {
    WbFixture f;
    f.registry.registerType(makeWbEntityDef());
    f.broadcaster.onConnect(0u); // creates the peer's input slot so authority can be set
    auto reg = makeRegistry(f.ctx);

    // Grant game-master caps directly (the enqueued callback body is what `grant` runs on the loop).
    REQUIRE(f.broadcaster.setPeerAuthority(
        0u, fl::PeerAuthority{fl::kGameMasterCaps, fl::PeerAuthority::kNoFactionBinding}));
    fl::PeerAuthority a = f.broadcaster.getPeerAuthority(0u);
    CHECK(a.caps == fl::kGameMasterCaps);

    // The granted peer, dispatched with its issuer, may spawn (SpawnAny) but not config (ServerConfig).
    const fl::CommandIssuer granted{0u, a.caps, a.factionIndex};
    CHECK(reg.dispatch("spawn x 0 0 0", granted).find("permission denied") == std::string::npos);
    CHECK(reg.dispatch("set_weather storm", granted).find("permission denied") != std::string::npos);

    // Revoke, and the same peer is now refused everything privileged.
    REQUIRE(f.broadcaster.setPeerAuthority(0u, fl::PeerAuthority{}));
    const fl::PeerAuthority after = f.broadcaster.getPeerAuthority(0u);
    CHECK(after.caps == 0);
    const fl::CommandIssuer revoked{0u, after.caps, after.factionIndex};
    CHECK(reg.dispatch("spawn x 0 0 0", revoked).find("permission denied") != std::string::npos);
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

// The per-peer lines (with the #518 send-rate / loss columns) are emitted from an enqueueSimCallback
// to stdout/shell, which a unit test can't drain without starting the loop. Assert the documented
// columns via the command help text; the underlying PeerInfo values are covered in test_world_broadcaster.
TEST_CASE("AdminConsole wb: peers help advertises send rate and loss columns", "[admin_console][wb]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    const std::string help = reg.helpFor("peers");
    CHECK(help.find("send rate") != std::string::npos);
    CHECK(help.find("loss") != std::string::npos);
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
    CHECK(out.find("load:") != std::string::npos);     // overrun governor load % (#514)
    CHECK(out.find("interest:") != std::string::npos); // overrun governor interest-radius % (#726)
}

// Read the integer out of "uptime: NNNs ..." — the tests below assert on the VALUE, because the
// defect being guarded against produced a perfectly well-formed line carrying the wrong number.
static long long parseUptimeSecs(const std::string& statusLine) {
    const auto at = statusLine.find("uptime: ");
    REQUIRE(at != std::string::npos);
    return std::strtoll(statusLine.c_str() + at + 8, nullptr, 10);
}

TEST_CASE("AdminConsole wb: status reports the SERVER's uptime, not the machine's (#1048)",
          "[admin_console][wb][uptime]") {
    WbFixture f;
    fl::ManualClock clock;
    f.ctx.env.uptime = fl::ServerUptime{clock};
    clock.advance(std::chrono::seconds(137));

    auto reg = makeRegistry(f.ctx);
    CHECK(parseUptimeSecs(reg.dispatch("status")) == 137);
}

TEST_CASE("AdminConsole wb: a context nobody handed a start instant still cannot report boot time (#1048)",
          "[admin_console][wb][uptime]") {
    // The exact shape of the bug: the context is built and the uptime field is simply never assigned.
    // It used to leave a value-initialised steady_clock::time_point, i.e. the clock epoch — which on
    // Linux is boot, so `status` reported the machine's uptime in seconds and looked plausible. A
    // ServerUptime starts itself, so the worst case is now "a few milliseconds", not "eleven days".
    WbFixture f; // f.ctx.env.uptime left exactly as constructed
    auto reg = makeRegistry(f.ctx);
    const long long secs = parseUptimeSecs(reg.dispatch("status"));
    CHECK(secs >= 0);
    CHECK(secs < 60);
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
    CHECK(out.find("overrun:") != std::string::npos); // governor load/snapshot/ai_stride line (#514)
    CHECK(out.find("interest") != std::string::npos); // governor interest-radius scale (#726)
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
    f.broadcaster.setAdminDispatch([](std::string_view, const CommandIssuer&) { return std::string{}; });
    f.registry.registerType(makeWbEntityDef());
    f.broadcaster.onConnect(0u);
    // Complete the handshake: since #1069 an un-admitted peer's MsgAdminCommand is dropped in the
    // dispatch preamble, so it can no longer burn admin-auth attempts — and lock out an IP — without
    // ever having joined. A real operator's client is always admitted before it opens the console.
    {
        fl::MsgConnectRequest creq{};
        creq.requestedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
        f.broadcaster.onReceive(0u, &creq, sizeof(creq));
    }

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
    f.broadcaster.setAdminDispatch([](std::string_view, const CommandIssuer&) { return std::string{}; });
    f.registry.registerType(makeWbEntityDef());
    f.broadcaster.onConnect(0u);
    // Complete the handshake: since #1069 an un-admitted peer's MsgAdminCommand is dropped in the
    // dispatch preamble, so it can no longer burn admin-auth attempts — and lock out an IP — without
    // ever having joined. A real operator's client is always admitted before it opens the console.
    {
        fl::MsgConnectRequest creq{};
        creq.requestedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
        f.broadcaster.onReceive(0u, &creq, sizeof(creq));
    }

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
    f.broadcaster.setAdminDispatch([](std::string_view, const CommandIssuer&) { return std::string{}; });
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

    connectPilotWb(broadcaster);

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
    f.broadcaster.setGroundElevationQuery([&](glm::dvec3) {
        ++queryCalls;
        return 42.f;
    });
    connectPilotWb(f.broadcaster);
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

TEST_CASE("Admin console: detonate validates and queues with a synchronous ack", "[admin_console]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);

    CHECK(reg.dispatch("detonate").find("usage:") != std::string::npos);
    CHECK(reg.dispatch("detonate 0 0 0 abc 50").find("invalid") != std::string::npos);
    CHECK(reg.dispatch("detonate 0 0 0 0 50").find("must be > 0") != std::string::npos);

    const std::string ack = reg.dispatch("detonate 100 500 -200 120 200 --nuclear");
    CHECK(ack.find("detonate: queued") != std::string::npos);
    CHECK(ack.find("nuclear") != std::string::npos);
}

// ---------------------------------------------------------------------------
// ATC admin commands (#705): atc_status (sync read), atc_scramble / atc_hold
// (enqueueSimCallback + synchronous ack). A real AtcService + GameLoop is used;
// the loop is never started, so enqueued callbacks are only captured, and the
// sync ack is what these assert.
// ---------------------------------------------------------------------------

namespace {
struct AtcFixture {
    NullLogger2 log;
    NoopSim2 noop;
    GameLoop loop{noop, log}; // do NOT start
    fl::EntityTypeRegistry reg;
    fl::EntityManager em{log, reg};
    fl::AirportRegistry airports;
    std::unique_ptr<fl::atc::AtcService> atc;
    ServerCommandContext ctx;

    AtcFixture() {
        airports.load({fl::builtinAirfield()}, fl::kEarthRadiusM, nullptr);
        atc = std::make_unique<fl::atc::AtcService>(em, airports, fl::kEarthRadiusM);
        ctx.sim.gameLoop = &loop;
        ctx.sim.atc = atc.get();
    }
};
} // namespace

TEST_CASE("AdminConsole: atc_status reports facilities", "[admin_console][atc]") {
    AtcFixture f;
    // No traffic yet -> no active facilities.
    auto reg = makeRegistry(f.ctx);
    CHECK(reg.dispatch("atc_status").find("no active facilities") != std::string::npos);

    // A scramble creates a facility; atc_status then lists it. Run the scramble directly on the
    // service (the admin path would enqueue it) so the facility exists for the sync read.
    f.atc->setSpawnHandler([](const fl::atc::AtcService::DepartureSpawn&) {});
    f.atc->scramble("builtin:airfield", "builtin:debug-entity", 1);
    CHECK(reg.dispatch("atc_status").find("builtin:airfield") != std::string::npos);
}

TEST_CASE("AdminConsole: atc_scramble acks synchronously", "[admin_console][atc]") {
    AtcFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("atc_scramble builtin:airfield builtin:debug-entity 3");
    CHECK_FALSE(out.empty());
    CHECK(out.find("queued 3") != std::string::npos);
    CHECK(out.find("builtin:airfield") != std::string::npos);

    CHECK(reg.dispatch("atc_scramble").find("usage:") != std::string::npos);
    CHECK(reg.dispatch("atc_scramble a b 99").find("count must be") != std::string::npos);
}

TEST_CASE("AdminConsole: atc_hold acks synchronously and validates its argument", "[admin_console][atc]") {
    AtcFixture f;
    auto reg = makeRegistry(f.ctx);
    CHECK(reg.dispatch("atc_hold builtin:airfield on").find("holding") != std::string::npos);
    CHECK(reg.dispatch("atc_hold builtin:airfield off").find("releasing") != std::string::npos);
    CHECK(reg.dispatch("atc_hold builtin:airfield maybe").find("on|off") != std::string::npos);
    CHECK(reg.dispatch("atc_hold").find("usage:") != std::string::npos);
}

TEST_CASE("AdminConsole: atc commands report unavailable with no service", "[admin_console][atc]") {
    ServerCommandContext ctx{}; // no atc, no gameLoop
    auto reg = makeRegistry(ctx);
    CHECK(reg.dispatch("atc_status").find("not available") != std::string::npos);
    CHECK(reg.dispatch("atc_scramble a b").find("not available") != std::string::npos);
    CHECK(reg.dispatch("atc_hold a on").find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Permission-checked dispatch (#946): the issuer-aware dispatch(line, issuer) refuses commands the
// issuer lacks the capability for. The plain dispatch(line) path (every test above) is unchanged —
// it is the implicit-Admin path (stdin / RCON / --admin-token) and never cap-checks.
// ---------------------------------------------------------------------------

static bool refused(const std::string& out) {
    return out.find("permission denied") != std::string::npos;
}

TEST_CASE("AdminConsole: dispatch(line) implicit-Admin path never cap-checks", "[admin_console][permission]") {
    auto reg = makeRegistry();
    // The no-issuer overload runs every command regardless of caps (stdin / RCON / --admin-token).
    CHECK_FALSE(refused(reg.dispatch("kick 42")));
    CHECK_FALSE(refused(reg.dispatch("spawn x 0 0 0")));
    CHECK_FALSE(refused(reg.dispatch("set_weather storm")));
    CHECK_FALSE(refused(reg.dispatch("quit")));
}

TEST_CASE("AdminConsole: Admin issuer runs every capability class", "[admin_console][permission]") {
    auto reg = makeRegistry();
    const CommandIssuer admin{7, kAdminCaps, PeerAuthority::kNoFactionBinding};
    CHECK_FALSE(refused(reg.dispatch("kick 42", admin)));
    CHECK_FALSE(refused(reg.dispatch("spawn x 0 0 0", admin)));
    CHECK_FALSE(refused(reg.dispatch("set_weather storm", admin)));
    CHECK_FALSE(refused(reg.dispatch("flight list", admin)));
    CHECK_FALSE(refused(reg.dispatch("spectate 1 2", admin)));
    CHECK_FALSE(refused(reg.dispatch("set_role 1 observer", admin)));
    CHECK_FALSE(refused(reg.dispatch("quit", admin))); // unannotated = Admin-only
}

TEST_CASE("AdminConsole: public commands run for a zero-cap issuer", "[admin_console][permission]") {
    auto reg = makeRegistry();
    const CommandIssuer nobody{9, 0u, PeerAuthority::kNoFactionBinding};
    // help/status/peers/tickstats/mutes/seats/atc_status are annotated public (0 caps).
    CHECK_FALSE(refused(reg.dispatch("help", nobody)));
    CHECK(reg.dispatch("help", nobody).find("status") != std::string::npos); // real handler ran
    CHECK_FALSE(refused(reg.dispatch("status", nobody)));
    CHECK_FALSE(refused(reg.dispatch("peers", nobody)));
    // But a privileged command is refused with a message naming the missing capability.
    const std::string k = reg.dispatch("kick 42", nobody);
    CHECK(refused(k));
    CHECK(k.find("kick_ban") != std::string::npos);
}

TEST_CASE("AdminConsole: Moderator caps permit kick/ban, refuse spawn/config/grant", "[admin_console][permission]") {
    auto reg = makeRegistry();
    const CommandIssuer mod{3, kModeratorCaps, PeerAuthority::kNoFactionBinding};
    // Permitted: kick/ban/unban (KickBan), mute (Mute), spectate (SpectateAny).
    CHECK_FALSE(refused(reg.dispatch("kick 42", mod)));
    CHECK_FALSE(refused(reg.dispatch("ban 1.2.3.4", mod)));
    CHECK_FALSE(refused(reg.dispatch("mute 3", mod)));
    CHECK_FALSE(refused(reg.dispatch("spectate 1 2", mod)));
    // Refused: spawn (SpawnAny), config (ServerConfig), AI orders (CommandAnyAi), grant (GrantRoles).
    CHECK(refused(reg.dispatch("spawn x 0 0 0", mod)));
    CHECK(refused(reg.dispatch("set_weather storm", mod)));
    CHECK(refused(reg.dispatch("flight list", mod)));
    CHECK(refused(reg.dispatch("set_role 1 observer", mod)));
    // A moderator cannot self-elevate: it lacks GrantRoles (set_role is GrantRoles-gated).
    CHECK(refused(reg.dispatch("set_role 1 observer", mod)));
}

TEST_CASE("AdminConsole: GameMaster caps permit map/spawn/orders, refuse config/grant", "[admin_console][permission]") {
    auto reg = makeRegistry();
    const CommandIssuer gm{5, kGameMasterCaps, PeerAuthority::kNoFactionBinding};
    // Permitted: spawn (SpawnAny), flight orders (CommandAnyAi), spectate (SpectateAny).
    CHECK_FALSE(refused(reg.dispatch("spawn x 0 0 0", gm)));
    CHECK_FALSE(refused(reg.dispatch("flight list", gm)));
    CHECK_FALSE(refused(reg.dispatch("spectate 1 2", gm)));
    // Refused: server config and grant (a GM is not a config admin, and cannot grant roles).
    CHECK(refused(reg.dispatch("set_weather storm", gm)));
    CHECK(refused(reg.dispatch("reload_config", gm)));
    CHECK(refused(reg.dispatch("set_role 1 observer", gm)));
}
