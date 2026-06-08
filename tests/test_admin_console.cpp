// SPDX-License-Identifier: GPL-3.0-or-later
#include "AdminConsole.h"
#include <debug/DebugCommandRegistry.h>

#include <catch2/catch_test_macros.hpp>
#include <csignal>
#include <string>

// Build a registry with an all-null context (except the fields being exercised).
static DebugCommandRegistry makeRegistry(ServerCommandContext ctx = {}) {
    DebugCommandRegistry reg;
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
    ctx.quitFlag = &flag;
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
    ctx.banlistPath = &emptyPath; // non-null but empty
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
    ctx.allowlistPath = &emptyPath;
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
    ctx.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.gameLoop = nullptr;
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --in 30m --force");
    CHECK(out.find("not available") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --in with invalid duration returns error", "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    static int sentinel;
    ctx.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --in notaduration --force");
    CHECK(out.find("invalid") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --in without --force returns confirmation prompt", "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    static int sentinel;
    ctx.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    ctx.shutdownRequireConfirm = true;
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --in 30m");
    // Should prompt to re-run with --force, not schedule anything
    CHECK(out.find("--force") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --now without --force returns confirmation prompt", "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    static int sentinel;
    ctx.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    ctx.shutdownRequireConfirm = true;
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
    ctx.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --bogus");
    CHECK(out.find("unknown flag") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --reason without value returns error", "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    static int sentinel;
    ctx.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --in 30m --force --reason");
    CHECK(out.find("requires a value") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --in with multi-word --reason preserves confirmation prompt",
          "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    static int sentinel;
    ctx.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    ctx.shutdownRequireConfirm = true;
    auto reg = makeRegistry(ctx);
    std::string out = reg.dispatch("shutdown --in 30m --reason scheduled maintenance");
    CHECK(out.find("--force") != std::string::npos);
}

TEST_CASE("AdminConsole: shutdown --reason stops consuming at next double-dash flag", "[admin_console][shutdown]") {
    ServerCommandContext ctx;
    static int sentinel;
    ctx.broadcaster = reinterpret_cast<fl::WorldBroadcaster*>(&sentinel);
    ctx.gameLoop = reinterpret_cast<GameLoop*>(&sentinel);
    ctx.shutdownRequireConfirm = false;
    ctx.minShutdownDelayS = 100;
    auto reg = makeRegistry(ctx);
    // --reason consumes "maintenance" only (stops at --force); --force bypasses the confirm gate;
    // the 10s delay is below minShutdownDelayS=100 so the min-delay gate fires.
    std::string out = reg.dispatch("shutdown --in 10s --reason maintenance --force");
    CHECK(out.find("at least") != std::string::npos);
}
