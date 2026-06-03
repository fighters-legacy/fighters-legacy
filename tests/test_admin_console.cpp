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
