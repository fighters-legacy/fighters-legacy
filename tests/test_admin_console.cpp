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
#include <filesystem>
#include <fstream>
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
    std::string out = reg.dispatch("help", systemIssuer());
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
    std::string out = reg.dispatch("xyzzy", systemIssuer());
    CHECK(out.find("unknown command") != std::string::npos);
}

// ---------------------------------------------------------------------------
// status — null context
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: status with null broadcaster returns error", "[admin_console]") {
    auto reg = makeRegistry(); // broadcaster == nullptr
    std::string out = reg.dispatch("status", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// kick — argument parsing
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: kick with no args returns usage", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("kick", systemIssuer());
    CHECK(out.find("usage") != std::string::npos);
}

TEST_CASE("AdminConsole: kick with non-numeric arg and null broadcaster returns error", "[admin_console]") {
    auto reg = makeRegistry(); // broadcaster == nullptr
    std::string out = reg.dispatch("kick 1.2.3.4", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: kick with numeric arg and null broadcaster returns error", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("kick 42", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// ban / unban — argument parsing
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: ban with no args returns usage", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("ban", systemIssuer());
    CHECK(out.find("usage") != std::string::npos);
}

TEST_CASE("AdminConsole: ban with null broadcaster returns error", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("ban 1.2.3.4", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: unban with no args returns usage", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("unban", systemIssuer());
    CHECK(out.find("usage") != std::string::npos);
}

TEST_CASE("AdminConsole: unban with null broadcaster returns error", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("unban 1.2.3.4", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// mute / unmute / mutes (#646)
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: mute with no args returns usage", "[admin_console]") {
    auto reg = makeRegistry();
    CHECK(reg.dispatch("mute", systemIssuer()).find("usage") != std::string::npos);
    CHECK(reg.dispatch("unmute", systemIssuer()).find("usage") != std::string::npos);
}

TEST_CASE("AdminConsole: mute with null broadcaster returns not available", "[admin_console]") {
    auto reg = makeRegistry();
    CHECK(reg.dispatch("mute 3", systemIssuer()).find("not available") != std::string::npos);
    CHECK(reg.dispatch("mutes", systemIssuer()).find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: admin_unlock with no args returns usage", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("admin_unlock", systemIssuer());
    CHECK(out.find("usage") != std::string::npos);
}

TEST_CASE("AdminConsole: admin_unlock with null broadcaster returns not available", "[admin_console]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("admin_unlock 1.2.3.4", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// set_time — range validation
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: set_time out-of-range returns error", "[admin_console]") {
    auto reg = makeRegistry();
    CHECK(reg.dispatch("set_time 25", systemIssuer()).find("must be in") != std::string::npos);
    CHECK(reg.dispatch("set_time -1", systemIssuer()).find("must be in") != std::string::npos);
}

TEST_CASE("AdminConsole: set_time with no args returns usage", "[admin_console]") {
    auto reg = makeRegistry();
    CHECK(reg.dispatch("set_time", systemIssuer()).find("usage") != std::string::npos);
}

// ---------------------------------------------------------------------------
// reload_config — null context
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: reload_config with null configPath returns error", "[admin_console]") {
    auto reg = makeRegistry(); // configPath == nullptr
    std::string out = reg.dispatch("reload_config", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: reload_config without a running config to diff against says so", "[admin_console]") {
    ServerCommandContext ctx;
    std::string path = "/nonexistent/server.toml";
    ctx.env.configPath = &path; // but env.runningConfig is null
    auto reg = makeRegistry(ctx);
    CHECK(reg.dispatch("reload_config", systemIssuer()).find("not available") != std::string::npos);
}

namespace {

// Writes a server.toml the reload_config tests can point at. Removed on destruction so a failing
// test does not leave one behind for the next run to read.
struct TempConfig {
    std::filesystem::path path;
    explicit TempConfig(std::string_view body, std::string_view stem) {
        path = std::filesystem::temp_directory_path() / (std::string("fl_") + std::string(stem) + ".toml");
        std::ofstream(path) << body;
    }
    ~TempConfig() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::string str() const {
        return path.string();
    }
};

} // namespace

TEST_CASE("AdminConsole: reload_config names an edited restart-only key instead of dropping it",
          "[admin_console][config_reload]") {
    // The reported failure: an operator edits a restart-only key and gets silence, so they cannot
    // tell "applied" from "ignored" (#1081).
    TempConfig file("[server]\nport = 4779\n", "reload_restart_only");
    ServerConfig running; // port 4778
    ServerCommandContext ctx;
    std::string path = file.str();
    ctx.env.configPath = &path;
    ctx.env.runningConfig = &running;
    auto reg = makeRegistry(ctx);

    const std::string out = reg.dispatch("reload_config", systemIssuer());
    CHECK(out.find("restart required for 1 changed key(s)") != std::string::npos);
    CHECK(out.find("server.port 4778 -> 4779") != std::string::npos);
}

TEST_CASE("AdminConsole: reload_config reports the hot keys that changed", "[admin_console][config_reload]") {
    TempConfig file("[world]\ndraw_distance_km = 50.0\n", "reload_hot");
    ServerConfig running; // draw_distance_km 100
    ServerCommandContext ctx;
    std::string path = file.str();
    ctx.env.configPath = &path;
    ctx.env.runningConfig = &running;
    auto reg = makeRegistry(ctx);

    const std::string out = reg.dispatch("reload_config", systemIssuer());
    CHECK(out.find("changed: world.draw_distance_km 100 -> 50") != std::string::npos);
    CHECK(out.find("restart required") == std::string::npos);
}

TEST_CASE("AdminConsole: reload_config of an unedited file reports neither list", "[admin_console][config_reload]") {
    TempConfig file("[server]\nname = \"Unnamed Server\"\n", "reload_noop");
    ServerConfig running;
    ServerCommandContext ctx;
    std::string path = file.str();
    ctx.env.configPath = &path;
    ctx.env.runningConfig = &running;
    auto reg = makeRegistry(ctx);

    const std::string out = reg.dispatch("reload_config", systemIssuer());
    CHECK(out.find("hot key(s)") != std::string::npos);
    CHECK(out.find("changed:") == std::string::npos);
    CHECK(out.find("restart required") == std::string::npos);
}

TEST_CASE("AdminConsole: reload_config refuses a file with a syntax error rather than applying defaults",
          "[admin_console][config_reload]") {
    // parseServerConfig answers a syntax error with a DEFAULT config, which is indistinguishable from
    // a valid file that sets nothing. Applying it would reset the live MOTD, draw distance and
    // congestion levers because the operator mistyped a bracket -- and report success.
    TempConfig file("[world]\ndraw_distance_km = 42.0\n[world]\n", "reload_broken"); // duplicate table
    ServerConfig running;
    ServerCommandContext ctx;
    std::string path = file.str();
    ctx.env.configPath = &path;
    ctx.env.runningConfig = &running;
    auto reg = makeRegistry(ctx);

    const std::string out = reg.dispatch("reload_config", systemIssuer());
    CHECK(out.find("syntax error") != std::string::npos);
    CHECK(out.find("nothing was applied") != std::string::npos);
    CHECK(out.find("hot key(s)") == std::string::npos);
}

TEST_CASE("AdminConsole: reload_config never prints a credential", "[admin_console][config_reload]") {
    TempConfig file("[security]\noperator_password = \"hunter2\"\n", "reload_secret");
    ServerConfig running;
    ServerCommandContext ctx;
    std::string path = file.str();
    ctx.env.configPath = &path;
    ctx.env.runningConfig = &running;
    auto reg = makeRegistry(ctx);

    const std::string out = reg.dispatch("reload_config", systemIssuer());
    CHECK(out.find("security.operator_password <unset> -> <set>") != std::string::npos);
    CHECK(out.find("hunter2") == std::string::npos);
}

// ---------------------------------------------------------------------------
// quit — sets quitFlag
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: quit sets quitFlag to 1", "[admin_console]") {
    volatile sig_atomic_t flag = 0;
    ServerCommandContext ctx;
    ctx.env.quitFlag = &flag;
    auto reg = makeRegistry(ctx);
    auto out = reg.dispatch("quit", systemIssuer());
    CHECK(flag == 1);
    CHECK(!out.empty());
}

TEST_CASE("AdminConsole: quit with null quitFlag returns error", "[admin_console]") {
    auto reg = makeRegistry(); // quitFlag == nullptr
    std::string out = reg.dispatch("quit", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// reload_banlist
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: reload_banlist with null banlistPath returns not available", "[admin_console][security]") {
    auto reg = makeRegistry(); // banlistPath == nullptr
    std::string out = reg.dispatch("reload_banlist", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: reload_banlist with empty banlistPath returns not available", "[admin_console][security]") {
    ServerCommandContext ctx;
    std::string emptyPath;
    ctx.bans.banlistPath = &emptyPath; // non-null but empty
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("reload_banlist", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// reload_allowlist
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: reload_allowlist with null allowlistPath returns not available", "[admin_console][security]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("reload_allowlist", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: reload_allowlist with empty allowlistPath returns not available",
          "[admin_console][security]") {
    ServerCommandContext ctx;
    std::string emptyPath;
    ctx.bans.allowlistPath = &emptyPath;
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("reload_allowlist", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// ban / unban — null saveBanlist does not crash
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: ban with null broadcaster returns not available", "[admin_console][security]") {
    auto reg = makeRegistry(); // broadcaster == nullptr
    std::string out = reg.dispatch("ban 1.2.3.4", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: unban with null broadcaster returns not available", "[admin_console][security]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("unban 1.2.3.4", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// shutdown command
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: shutdown with null broadcaster returns not available", "[admin_console][shutdown]") {
    auto reg = makeRegistry(); // broadcaster == nullptr
    std::string out = reg.dispatch("shutdown --in 30m --force", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --in with null gameLoop returns not available", "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    // broadcaster non-null but gameLoop == nullptr — use a sentinel address
    static int sentinel;
    ctx.sim.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.sim.gameLoop = nullptr;
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --in 30m --force", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --in with invalid duration returns error", "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    static int sentinel;
    ctx.sim.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.sim.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --in notaduration --force", systemIssuer());
    CHECK(out.find("invalid") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --in without --force returns confirmation prompt", "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    static int sentinel;
    ctx.sim.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.sim.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    ctx.shutdown.requireConfirm = true;
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --in 30m", systemIssuer());
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
    std::string out = reg.dispatch("shutdown --now", systemIssuer());
    CHECK(out.find("--force") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --cancel with null broadcaster returns not available", "[admin_console][shutdown]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("shutdown --cancel", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --delay with null broadcaster returns not available", "[admin_console][shutdown]") {
    auto reg = makeRegistry();
    std::string out = reg.dispatch("shutdown --delay 5m", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown unknown flag returns error", "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    static int sentinel;
    ctx.sim.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.sim.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --bogus", systemIssuer());
    CHECK(out.find("unknown flag") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --reason without value returns error", "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    static int sentinel;
    ctx.sim.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.sim.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --in 30m --force --reason", systemIssuer());
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
    std::string out = reg.dispatch("shutdown --in 30m --reason scheduled maintenance", systemIssuer());
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
    std::string out = reg.dispatch("shutdown --in 10s --reason maintenance --force", systemIssuer());
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
    fl::AdminChannelRegistry channels;
    fl::AdminChannel enet{[](std::string_view, const fl::CommandIssuer&) { return std::string{}; },
                          fl::AdminChannel::Config{"enet"}, fl::SystemClock::instance()};
    ServerCommandContext ctx;

    AsyncAckFixture() {
        ctx.sim.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&bcast_sentinel);
        ctx.sim.entityManager = reinterpret_cast<fl::EntityManager*>(&em_sentinel);
        ctx.sim.gameLoop = &loop;
        channels.add(enet);
        ctx.adminChannels = &channels;
    }
};
int AsyncAckFixture::bcast_sentinel = 0;
int AsyncAckFixture::em_sentinel = 0;
} // namespace

TEST_CASE("AdminConsole async ack: kick numeric peer returns non-empty ack with id", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("kick 42", systemIssuer());
    CHECK_FALSE(out.empty());
    CHECK(out.find("42") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: mute non-numeric arg rejected (#646)", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    CHECK(reg.dispatch("mute notanid", systemIssuer()).find("expected a peer ID") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: mute/unmute/mutes return a sync ack (#646)", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    CHECK(reg.dispatch("mute 7", systemIssuer()).find("queued peer 7") != std::string::npos);
    CHECK(reg.dispatch("unmute 7", systemIssuer()).find("queued peer 7") != std::string::npos);
    CHECK(reg.dispatch("mutes", systemIssuer()) == "mutes: queued");
}

TEST_CASE("AdminConsole: spectate usage / not-available (#403)", "[admin_console]") {
    auto reg = makeRegistry();
    CHECK(reg.dispatch("spectate", systemIssuer()).find("usage") != std::string::npos);
    CHECK(reg.dispatch("spectate 3", systemIssuer()).find("usage") != std::string::npos);
    CHECK(reg.dispatch("spectate 3 5", systemIssuer()).find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: spectate parses target + off (#403)", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    CHECK(reg.dispatch("spectate 3 5", systemIssuer()).find("queued peer 3") != std::string::npos);
    CHECK(reg.dispatch("spectate 3 off", systemIssuer()).find("queued peer 3") != std::string::npos);
    CHECK(reg.dispatch("spectate 3 notanumber", systemIssuer()).find("number or 'off'") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: kick IP returns non-empty ack with address", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("kick 1.2.3.4", systemIssuer());
    CHECK_FALSE(out.empty());
    CHECK(out.find("1.2.3.4") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: ban IP returns non-empty ack", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("ban 1.2.3.4", systemIssuer());
    CHECK_FALSE(out.empty());
}

TEST_CASE("AdminConsole async ack: unban IP returns non-empty ack with address", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("unban 1.2.3.4", systemIssuer());
    CHECK_FALSE(out.empty());
    CHECK(out.find("1.2.3.4") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: admin_unlock IP returns non-empty ack", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_unlock 1.2.3.4", systemIssuer());
    CHECK_FALSE(out.empty());
    CHECK(out.find("1.2.3.4") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: admin_unlock with several channels registered returns ack with IP",
          "[admin_console][async_ack]") {
    AsyncAckFixture f;
    fl::AdminChannel rcon([](std::string_view, const fl::CommandIssuer&) { return std::string{}; },
                          fl::AdminChannel::Config{"rcon"}, fl::SystemClock::instance());
    f.channels.add(rcon);
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_unlock 1.2.3.4", systemIssuer());
    CHECK_FALSE(out.empty());
    CHECK(out.find("1.2.3.4") != std::string::npos);
}

TEST_CASE("AdminConsole: admin_unlock help text promises every channel, not a fixed list", "[admin_console]") {
    auto reg = makeRegistry();
    std::string help = reg.helpFor("admin_unlock");
    CHECK(help.find("every admin channel") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: spawn returns non-empty ack with type", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("spawn builtin:debug-entity 0 100 0", systemIssuer());
    CHECK_FALSE(out.empty());
    CHECK(out.find("builtin:debug-entity") != std::string::npos);
}

TEST_CASE("AdminConsole: spawn --ai lua returns error when loadAIScript is null", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    f.ctx.env.loadAIScript = nullptr;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("spawn builtin:debug-entity 0 100 0 --ai lua patrol", systemIssuer());
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
    std::string out = reg.dispatch("spawn builtin:debug-entity 0 100 0 --ai lua patrol", systemIssuer());
    CHECK_FALSE(out.empty());
    CHECK(out.find("not found") != std::string::npos);
}

TEST_CASE("AdminConsole: spawn --ai lua returns non-empty ack when script valid", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    f.ctx.env.loadAIScript = [](std::string_view) -> std::pair<std::string, std::string> {
        return {"function compute_control(s,t,dt) return {} end", ""};
    };
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("spawn builtin:debug-entity 0 100 0 --ai lua patrol", systemIssuer());
    CHECK_FALSE(out.empty());
    CHECK(out.find("builtin:debug-entity") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: kill returns non-empty ack with index", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("kill 1", systemIssuer());
    CHECK_FALSE(out.empty());
    CHECK(out.find("1") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: tp returns non-empty ack with entity index", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("tp 1 0 100 0", systemIssuer());
    CHECK_FALSE(out.empty());
    CHECK(out.find("1") != std::string::npos);
}

TEST_CASE("AdminConsole async ack: shutdown no-args returns status queued string", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("shutdown", systemIssuer());
    CHECK(out == "shutdown: status queued");
}

TEST_CASE("AdminConsole async ack: shutdown --delay returns extension queued string", "[admin_console][async_ack]") {
    AsyncAckFixture f;
    f.ctx.shutdown.requireConfirm = false;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("shutdown --delay 5m", systemIssuer());
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
    (void)reg.dispatch("kick 42", systemIssuer());
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
    std::string out = reg.dispatch("pause", systemIssuer());
    CHECK_FALSE(out.empty());
    CHECK(f.loop.rate() == TimeRate::Paused);
}

TEST_CASE("AdminConsole: resume sets GameLoop rate to Normal", "[admin_console][pause]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);
    (void)reg.dispatch("pause", systemIssuer());
    CHECK(f.loop.rate() == TimeRate::Paused);
    std::string out = reg.dispatch("resume", systemIssuer());
    CHECK_FALSE(out.empty());
    CHECK(f.loop.rate() == TimeRate::Normal);
}

TEST_CASE("AdminConsole: pause with null gameLoop returns error message", "[admin_console][pause]") {
    ServerCommandContext ctx{};
    ctx.sim.gameLoop = nullptr;
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("pause", systemIssuer());
    CHECK_FALSE(out.empty()); // should return "not available" or similar, not crash
}

TEST_CASE("AdminConsole: resume with null gameLoop returns error message", "[admin_console][pause]") {
    ServerCommandContext ctx{};
    ctx.sim.gameLoop = nullptr;
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("resume", systemIssuer());
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
    NoopSim2 noop;
    GameLoop loop{noop, log}; // do NOT call loop.start()
    fl::AdminChannelRegistry channels;
    std::vector<std::unique_ptr<fl::AdminChannel>> owned;
    ServerCommandContext ctx;

    // An admin channel is frozen at construction now (#1082), so which frontends this fixture wires is
    // a CONSTRUCTOR argument rather than something a test switches on afterwards. `enetMaxFailures`
    // of 0 means "no ENet frontend", which is what leaves MsgAdminCommand handling off.
    std::unique_ptr<fl::WorldBroadcaster> bc;
    fl::AdminChannel* enetCh{nullptr};

    explicit WbFixture(int enetMaxFailures = 0, int enetLockoutSeconds = 300) {
        fl::WorldBroadcasterHooks hooks;
        if (enetMaxFailures > 0) {
            enetCh = &addChannel("enet", enetMaxFailures, enetLockoutSeconds);
            hooks.comms.adminChannel = enetCh;
        }
        bc = std::make_unique<fl::WorldBroadcaster>(em, registry, net, log, nullptr, fl::WorldQueries{},
                                                    std::move(hooks));
        ctx.sim.broadcaster = bc.get();
        ctx.sim.entityManager = &em;
        ctx.sim.gameLoop = &loop;
        ctx.adminChannels = &channels;
    }

    fl::WorldBroadcaster& broadcaster() {
        return *bc;
    }

    // The ENet channel this fixture wired, for a test that inspects its lockout directly.
    fl::AdminChannel& enet() {
        return *enetCh;
    }

    // Register a channel the way fl-server does. Kept explicit per test: which frontends exist is
    // exactly what admin_auth_status reports, so a fixture that always wired all six would make the
    // "an unregistered surface is invisible" tests vacuous.
    fl::AdminChannel& addChannel(std::string name, int maxFailures = 5, int lockoutSeconds = 300,
                                 bool perIpAuth = true) {
        fl::AdminChannel::Config c;
        c.name = std::move(name);
        c.maxAuthFailures = maxFailures;
        c.lockoutSeconds = lockoutSeconds;
        c.perIpAuth = perIpAuth;
        owned.push_back(std::make_unique<fl::AdminChannel>(
            [](std::string_view, const fl::CommandIssuer&) { return std::string{}; }, c, fl::SystemClock::instance()));
        channels.add(*owned.back());
        return *owned.back();
    }
};
} // namespace

TEST_CASE("AdminConsole: peers with null broadcaster returns not available", "[admin_console]") {
    auto reg = makeRegistry(); // broadcaster == nullptr
    std::string out = reg.dispatch("peers", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole wb: set_role validates and queues with a synchronous ack (#857)", "[admin_console][wb]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    CHECK(reg.dispatch("set_role", systemIssuer()).find("usage") != std::string::npos);
    CHECK(reg.dispatch("set_role 0", systemIssuer()).find("usage") != std::string::npos);
    CHECK(reg.dispatch("set_role x observer", systemIssuer()).find("invalid peer ID") != std::string::npos);
    CHECK(reg.dispatch("set_role 0 bogus", systemIssuer()).find("pilot") != std::string::npos);
    std::string ok = reg.dispatch("set_role 3 observer", systemIssuer());
    CHECK(ok.find("queued peer 3") != std::string::npos);
    CHECK(ok.find("observer") != std::string::npos);
}

TEST_CASE("AdminConsole wb: grant validates and queues with a synchronous ack (#947)",
          "[admin_console][wb][permission]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    CHECK(reg.dispatch("grant", systemIssuer()).find("usage") != std::string::npos);
    CHECK(reg.dispatch("grant 0", systemIssuer()).find("usage") != std::string::npos);
    CHECK(reg.dispatch("grant x gm", systemIssuer()).find("invalid peer ID") != std::string::npos);
    CHECK(reg.dispatch("grant 0 superuser", systemIssuer()).find("role must be") != std::string::npos);
    CHECK(reg.dispatch("grant 0 faction_leader nope", systemIssuer()).find("invalid faction") != std::string::npos);
    std::string ok = reg.dispatch("grant 3 gm", systemIssuer());
    CHECK(ok.find("queued peer 3") != std::string::npos);
    CHECK(ok.find("gm") != std::string::npos);
}

TEST_CASE("AdminConsole wb: revoke validates and queues with a synchronous ack (#947)",
          "[admin_console][wb][permission]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    CHECK(reg.dispatch("revoke", systemIssuer()).find("usage") != std::string::npos);
    CHECK(reg.dispatch("revoke x", systemIssuer()).find("invalid peer ID") != std::string::npos);
    CHECK(reg.dispatch("revoke 7", systemIssuer()).find("queued peer 7") != std::string::npos);
}

TEST_CASE("AdminConsole wb: grant -> act -> revoke -> refused end to end (#947)", "[admin_console][wb][permission]") {
    WbFixture f;
    f.registry.registerType(makeWbEntityDef());
    f.broadcaster().onConnect(0u); // creates the peer's input slot so authority can be set
    auto reg = makeRegistry(f.ctx);

    // Grant game-master caps directly (the enqueued callback body is what `grant` runs on the loop).
    REQUIRE(f.broadcaster().setPeerAuthority(
        0u, fl::PeerAuthority{fl::kGameMasterCaps, fl::PeerAuthority::kNoFactionBinding}));
    fl::PeerAuthority a = f.broadcaster().getPeerAuthority(0u);
    CHECK(a.caps == fl::kGameMasterCaps);

    // The granted peer, dispatched with its issuer, may spawn (SpawnAny) but not config (ServerConfig).
    const fl::CommandIssuer granted{0u, a.caps, a.factionIndex};
    CHECK(reg.dispatch("spawn x 0 0 0", granted).find("permission denied") == std::string::npos);
    CHECK(reg.dispatch("set_weather storm", granted).find("permission denied") != std::string::npos);

    // Revoke, and the same peer is now refused everything privileged.
    REQUIRE(f.broadcaster().setPeerAuthority(0u, fl::PeerAuthority{}));
    const fl::PeerAuthority after = f.broadcaster().getPeerAuthority(0u);
    CHECK(after.caps == 0);
    const fl::CommandIssuer revoked{0u, after.caps, after.factionIndex};
    CHECK(reg.dispatch("spawn x 0 0 0", revoked).find("permission denied") != std::string::npos);
}

TEST_CASE("AdminConsole wb: peers with null gameLoop returns not available", "[admin_console][wb]") {
    WbFixture f;
    f.ctx.sim.gameLoop = nullptr;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("peers", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole wb: peers with no connected peers returns 0 peer(s) connected", "[admin_console][wb]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("peers", systemIssuer());
    CHECK(out == "0 peer(s) connected");
}

TEST_CASE("AdminConsole wb: peers with one connected peer returns 1 peer(s) connected", "[admin_console][wb]") {
    WbFixture f;
    f.registry.registerType(makeWbEntityDef());
    f.broadcaster().onConnect(0u);

    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("peers", systemIssuer());
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
    std::string out = reg.dispatch("status", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole wb: status with zero peers contains peers: 0", "[admin_console][wb]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("status", systemIssuer());
    CHECK(out.find("peers: 0") != std::string::npos);
}

TEST_CASE("AdminConsole wb: status with one connected peer contains peers: 1", "[admin_console][wb]") {
    WbFixture f;
    f.registry.registerType(makeWbEntityDef());
    f.broadcaster().onConnect(0u);

    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("status", systemIssuer());
    CHECK(out.find("peers: 1") != std::string::npos);
}

TEST_CASE("AdminConsole wb: status shows the real tick Hz line", "[admin_console][wb]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("status", systemIssuer());
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
    CHECK(parseUptimeSecs(reg.dispatch("status", systemIssuer())) == 137);
}

TEST_CASE("AdminConsole wb: a context nobody handed a start instant still cannot report boot time (#1048)",
          "[admin_console][wb][uptime]") {
    // The exact shape of the bug: the context is built and the uptime field is simply never assigned.
    // It used to leave a value-initialised steady_clock::time_point, i.e. the clock epoch — which on
    // Linux is boot, so `status` reported the machine's uptime in seconds and looked plausible. A
    // ServerUptime starts itself, so the worst case is now "a few milliseconds", not "eleven days".
    WbFixture f; // f.ctx.env.uptime left exactly as constructed
    auto reg = makeRegistry(f.ctx);
    const long long secs = parseUptimeSecs(reg.dispatch("status", systemIssuer()));
    CHECK(secs >= 0);
    CHECK(secs < 60);
}

// ---------------------------------------------------------------------------
// tickstats command tests
// ---------------------------------------------------------------------------

TEST_CASE("AdminConsole: tickstats with null broadcaster returns not available", "[admin_console]") {
    auto reg = makeRegistry(); // broadcaster == nullptr
    std::string out = reg.dispatch("tickstats", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole wb: tickstats before any tick reports no samples", "[admin_console][wb]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("tickstats", systemIssuer());
    CHECK(out.find("no ticks sampled") != std::string::npos);
}

TEST_CASE("AdminConsole wb: tickstats reports per-phase rows after ticks", "[admin_console][wb]") {
    WbFixture f;
    f.registry.registerType(makeWbEntityDef());
    f.broadcaster().onConnect(0u);
    for (uint64_t tick = 1; tick <= 5; ++tick)
        f.broadcaster().onTick(1.0 / 60.0, tick);

    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("tickstats", systemIssuer());
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
    std::string out = reg.dispatch("admin_auth_status", systemIssuer());
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole wb: admin_auth_status with no lockouts returns section header", "[admin_console][wb]") {
    WbFixture f{/*enetMaxFailures=*/5};
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_auth_status", systemIssuer());
    CHECK(out == "[admin] enet channel:");
}

TEST_CASE("AdminConsole wb: status with no lockouts does not show lockout line", "[admin_console][wb]") {
    WbFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("status", systemIssuer());
    CHECK(out.find("admin auth lockouts") == std::string::npos);
}

TEST_CASE("AdminConsole wb: status and admin_auth_status reflect active lockout", "[admin_console][wb]") {
    WbFixture f{/*enetMaxFailures=*/1, /*enetLockoutSeconds=*/300};
    f.net.peerAddr = "1.2.3.4";
    f.broadcaster().setOperatorPassword("correct");
    f.registry.registerType(makeWbEntityDef());
    f.broadcaster().onConnect(0u);
    // Complete the handshake: since #1069 an un-admitted peer's MsgAdminCommand is dropped in the
    // dispatch preamble, so it can no longer burn admin-auth attempts — and lock out an IP — without
    // ever having joined. A real operator's client is always admitted before it opens the console.
    {
        fl::MsgConnectRequest creq{};
        creq.requestedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
        f.broadcaster().onReceive(0u, &creq, sizeof(creq));
    }

    fl::MsgAdminCommand cmd{};
    std::snprintf(cmd.token, sizeof(cmd.token), "%s", "wrongpass");
    std::snprintf(cmd.command, sizeof(cmd.command), "%s", "status");
    f.broadcaster().onReceive(0u, &cmd, sizeof(cmd));

    auto reg = makeRegistry(f.ctx);

    std::string statusOut = reg.dispatch("status", systemIssuer());
    CHECK(statusOut.find("admin auth lockouts: 1 active") != std::string::npos);
    CHECK(statusOut.find("use admin_auth_status") != std::string::npos);

    std::string authOut = reg.dispatch("admin_auth_status", systemIssuer());
    CHECK(authOut.find("enet channel:") != std::string::npos);
    CHECK(authOut.find("1.2.3.4") != std::string::npos);
    CHECK(authOut.find("locked out") != std::string::npos);
}

TEST_CASE("AdminConsole wb: admin_auth_status shows pending failure line", "[admin_console][wb]") {
    WbFixture f{/*enetMaxFailures=*/3, /*enetLockoutSeconds=*/300};
    f.net.peerAddr = "1.2.3.4";
    f.broadcaster().setOperatorPassword("correct");
    f.registry.registerType(makeWbEntityDef());
    f.broadcaster().onConnect(0u);
    // Complete the handshake: since #1069 an un-admitted peer's MsgAdminCommand is dropped in the
    // dispatch preamble, so it can no longer burn admin-auth attempts — and lock out an IP — without
    // ever having joined. A real operator's client is always admitted before it opens the console.
    {
        fl::MsgConnectRequest creq{};
        creq.requestedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
        f.broadcaster().onReceive(0u, &creq, sizeof(creq));
    }

    fl::MsgAdminCommand cmd{};
    std::snprintf(cmd.token, sizeof(cmd.token), "%s", "wrongpass");
    std::snprintf(cmd.command, sizeof(cmd.command), "%s", "status");
    f.broadcaster().onReceive(0u, &cmd, sizeof(cmd)); // 1 failure, threshold=3, no lockout

    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_auth_status", systemIssuer());
    CHECK(out.find("1.2.3.4") != std::string::npos);
    CHECK(out.find("1 failure(s)") != std::string::npos);
    CHECK(out.find("threshold: 3") != std::string::npos);
}

TEST_CASE("AdminConsole wb: admin_auth_status with no channels registered says so", "[admin_console][wb]") {
    WbFixture f; // no frontend wired
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_auth_status", systemIssuer());
    CHECK(out.find("no admin channels registered") != std::string::npos);
}

TEST_CASE("AdminConsole wb: admin_auth_status reports one section per registered channel", "[admin_console][wb]") {
    WbFixture f{/*enetMaxFailures=*/5};
    f.addChannel("rcon");
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_auth_status", systemIssuer());
    CHECK(out.find("enet channel:") != std::string::npos);
    CHECK(out.find("rcon channel:") != std::string::npos);
}

TEST_CASE("AdminConsole wb: admin_auth_status shows a channel added later without any code change",
          "[admin_console][wb]") {
    // The failure mode #1079 exists to fix: a frontend nobody remembered to name in these commands was
    // invisible to an operator during an incident. Registering it is now the whole wiring.
    WbFixture f{/*enetMaxFailures=*/5};
    fl::AdminChannel& seventh = f.addChannel("seventh-frontend");
    seventh.recordAuthResult("5.6.7.8", /*authenticated=*/false);
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_auth_status", systemIssuer());
    CHECK(out.find("seventh-frontend channel:") != std::string::npos);
    CHECK(out.find("5.6.7.8") != std::string::npos);
    CHECK(out.find("1 failure(s)") != std::string::npos);
}

TEST_CASE("AdminConsole wb: admin_auth_status shows a locked-out entry with its expiry", "[admin_console][wb]") {
    WbFixture f;
    fl::AdminChannel& rcon = f.addChannel("rcon", /*maxFailures=*/1, /*lockoutSeconds=*/120);
    CHECK(rcon.recordAuthResult("5.6.7.8", /*authenticated=*/false));
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_auth_status", systemIssuer());
    CHECK(out.find("rcon channel:") != std::string::npos);
    CHECK(out.find("5.6.7.8") != std::string::npos);
    CHECK(out.find("locked out") != std::string::npos);
}

TEST_CASE("AdminConsole wb: admin_auth_status shows pending failures under the channel's threshold",
          "[admin_console][wb]") {
    WbFixture f;
    fl::AdminChannel& rcon = f.addChannel("rcon", /*maxFailures=*/5);
    for (int i = 0; i < 3; ++i)
        CHECK_FALSE(rcon.recordAuthResult("9.10.11.12", /*authenticated=*/false));
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_auth_status", systemIssuer());
    CHECK(out.find("rcon channel:") != std::string::npos);
    CHECK(out.find("9.10.11.12") != std::string::npos);
    CHECK(out.find("3 failure(s)") != std::string::npos);
    CHECK(out.find("threshold: 5") != std::string::npos);
}

TEST_CASE("AdminConsole wb: a trusted local surface reports no per-IP auth rather than an empty section",
          "[admin_console][wb]") {
    // stdin and the mission `do:` sink present no credential. Printing a threshold they can never
    // reach would read as "protected" to an operator scanning this during an incident.
    WbFixture f;
    f.addChannel("stdin", 0, 0, /*perIpAuth=*/false);
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("admin_auth_status", systemIssuer());
    CHECK(out.find("stdin channel:") != std::string::npos);
    CHECK(out.find("no per-IP authentication") != std::string::npos);
    CHECK(out.find("threshold") == std::string::npos);
}

TEST_CASE("AdminConsole wb: status counts lockouts across every channel, not just the ENet one",
          "[admin_console][wb]") {
    // The old count read the broadcaster's tracker alone, so an operator locked out of RCON saw a
    // clean `status`.
    WbFixture f{/*enetMaxFailures=*/5};
    fl::AdminChannel& rcon = f.addChannel("rcon", /*maxFailures=*/1);
    CHECK(rcon.recordAuthResult("5.6.7.8", /*authenticated=*/false));
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("status", systemIssuer());
    CHECK(out.find("admin auth lockouts: 1 active") != std::string::npos);
}

TEST_CASE("AdminConsole wb: status shows no lockout line after admin_unlock clears it", "[admin_console][wb]") {
    WbFixture f{/*enetMaxFailures=*/1, /*enetLockoutSeconds=*/300};
    f.net.peerAddr = "1.2.3.4";
    fl::AdminChannel& enet = f.enet();
    f.broadcaster().setOperatorPassword("correct");
    f.registry.registerType(makeWbEntityDef());
    f.broadcaster().onConnect(0u);

    fl::MsgAdminCommand cmd{};
    std::snprintf(cmd.token, sizeof(cmd.token), "%s", "wrongpass");
    std::snprintf(cmd.command, sizeof(cmd.command), "%s", "status");
    f.broadcaster().onReceive(0u, &cmd, sizeof(cmd)); // triggers lockout

    enet.clearLockout("1.2.3.4");

    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("status", systemIssuer());
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

TEST_CASE("WorldBroadcaster: the ground-elevation query is called per entity during onTick", "[admin_console][wb]") {
    WbFixture f;
    f.registry.registerType(makeWbEntityDef());

    int queryCalls = 0;
    // Built here rather than through the fixture: a query is frozen at construction now (#1082).
    fl::WorldQueries queries;
    queries.groundElevation = [&](glm::dvec3) {
        ++queryCalls;
        return 42.f;
    };
    fl::WorldBroadcaster wb(f.em, f.registry, f.net, f.log, nullptr, std::move(queries));
    connectPilotWb(wb);
    wb.onTick(1.0 / 60.0, 1u);

    CHECK(queryCalls > 0);
}

TEST_CASE("AdminConsole: set_weather snow enqueues preset change", "[admin_console]") {
    AsyncAckFixture f;
    fl::WeatherController wc;
    f.ctx.sim.weatherController = &wc;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("set_weather snow", systemIssuer());
    CHECK(out.find("snow") != std::string::npos);
    CHECK(out.find("not available") == std::string::npos);
}

TEST_CASE("AdminConsole: set_weather blizzard enqueues preset change", "[admin_console]") {
    AsyncAckFixture f;
    fl::WeatherController wc;
    f.ctx.sim.weatherController = &wc;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("set_weather blizzard", systemIssuer());
    CHECK(out.find("blizzard") != std::string::npos);
    CHECK(out.find("not available") == std::string::npos);
}

TEST_CASE("Admin console: detonate validates and queues with a synchronous ack", "[admin_console]") {
    AsyncAckFixture f;
    auto reg = makeRegistry(f.ctx);

    CHECK(reg.dispatch("detonate", systemIssuer()).find("usage:") != std::string::npos);
    CHECK(reg.dispatch("detonate 0 0 0 abc 50", systemIssuer()).find("invalid") != std::string::npos);
    CHECK(reg.dispatch("detonate 0 0 0 0 50", systemIssuer()).find("must be > 0") != std::string::npos);

    const std::string ack = reg.dispatch("detonate 100 500 -200 120 200 --nuclear", systemIssuer());
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
    CHECK(reg.dispatch("atc_status", systemIssuer()).find("no active facilities") != std::string::npos);

    // A scramble creates a facility; atc_status then lists it. Run the scramble directly on the
    // service (the admin path would enqueue it) so the facility exists for the sync read.
    f.atc->setSpawnHandler([](const fl::atc::AtcService::DepartureSpawn&) {});
    f.atc->scramble("builtin:airfield", "builtin:debug-entity", 1);
    CHECK(reg.dispatch("atc_status", systemIssuer()).find("builtin:airfield") != std::string::npos);
}

TEST_CASE("AdminConsole: atc_scramble acks synchronously", "[admin_console][atc]") {
    AtcFixture f;
    auto reg = makeRegistry(f.ctx);
    std::string out = reg.dispatch("atc_scramble builtin:airfield builtin:debug-entity 3", systemIssuer());
    CHECK_FALSE(out.empty());
    CHECK(out.find("queued 3") != std::string::npos);
    CHECK(out.find("builtin:airfield") != std::string::npos);

    CHECK(reg.dispatch("atc_scramble", systemIssuer()).find("usage:") != std::string::npos);
    CHECK(reg.dispatch("atc_scramble a b 99", systemIssuer()).find("count must be") != std::string::npos);
}

TEST_CASE("AdminConsole: atc_hold acks synchronously and validates its argument", "[admin_console][atc]") {
    AtcFixture f;
    auto reg = makeRegistry(f.ctx);
    CHECK(reg.dispatch("atc_hold builtin:airfield on", systemIssuer()).find("holding") != std::string::npos);
    CHECK(reg.dispatch("atc_hold builtin:airfield off", systemIssuer()).find("releasing") != std::string::npos);
    CHECK(reg.dispatch("atc_hold builtin:airfield maybe", systemIssuer()).find("on|off") != std::string::npos);
    CHECK(reg.dispatch("atc_hold", systemIssuer()).find("usage:") != std::string::npos);
}

TEST_CASE("AdminConsole: atc commands report unavailable with no service", "[admin_console][atc]") {
    ServerCommandContext ctx{}; // no atc, no gameLoop
    auto reg = makeRegistry(ctx);
    CHECK(reg.dispatch("atc_status", systemIssuer()).find("not available") != std::string::npos);
    CHECK(reg.dispatch("atc_scramble a b", systemIssuer()).find("not available") != std::string::npos);
    CHECK(reg.dispatch("atc_hold a on", systemIssuer()).find("not available") != std::string::npos);
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
    CHECK_FALSE(refused(reg.dispatch("kick 42", systemIssuer())));
    CHECK_FALSE(refused(reg.dispatch("spawn x 0 0 0", systemIssuer())));
    CHECK_FALSE(refused(reg.dispatch("set_weather storm", systemIssuer())));
    CHECK_FALSE(refused(reg.dispatch("quit", systemIssuer())));
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
