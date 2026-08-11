// SPDX-License-Identifier: GPL-3.0-or-later
//
// The debug-console commands that need a wired context (#1145).
//
// test_game_console.cpp covers the shell and the "not available" guards, which is most of what you
// can reach with an empty CommandContext. The commands below only do anything once a serverCommand,
// a reload hook or an articulation hook is present — which is to say, only in the game — so their
// real behaviour was untested. `art` in particular had no coverage at all, and it is the tool that
// tells "the clip is wrong" apart from "the sim is wrong" (#841).

#include <catch2/catch_test_macros.hpp>

#include "console/CommandRegistry.h"
#include "console/ConsoleCommands.h"
#include "entity/EntityDef.h"
#include "entity/EntityTypeRegistry.h"
#include "render/ArtChannel.h"

#include <string>
#include <vector>

using namespace fl;

namespace {

// Records what the console forwarded. The exact text matters: it is admin-console syntax that a
// server parses, so a malformed line is a command that silently does nothing.
struct Forwarded {
    std::vector<std::string> lines;
    [[nodiscard]] const std::string& last() const {
        return lines.back();
    }
};

EntityTypeRegistry registryWithOneType() {
    EntityTypeRegistry reg;
    EntityDef def;
    def.id = "test:unit";
    def.name = "Unit";
    def.category = ObjectCategory::AirVehicle;
    def.maxHp = 100.f;
    reg.registerType(std::move(def));
    return reg;
}

} // namespace

// ---------------------------------------------------------------------------
// spawn
// ---------------------------------------------------------------------------

TEST_CASE("spawn: a type index is accepted as well as a type id (#1145)", "[console][commands]") {
    // `types` prints an index next to each id, so an operator reading that list will type the
    // number. Both spellings have to resolve or half the printed output is unusable.
    EntityTypeRegistry reg = registryWithOneType();
    Forwarded fwd;
    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.typeRegistry = &reg;
    ctx.serverCommand = [&](std::string_view s) { fwd.lines.emplace_back(s); };
    registerConsoleCommands(cmds, ctx);

    CHECK(cmds.dispatch("spawn 0 1 2 3", systemIssuer()).find("queued") != std::string::npos);
    CHECK(fwd.last() == "spawn 0 1 2 3");
}

TEST_CASE("spawn: an out-of-range index is rejected, not forwarded (#1145)", "[console][commands]") {
    // Forwarding it would make the server resolve a type that does not exist, one round trip later
    // and with no console output to explain it.
    EntityTypeRegistry reg = registryWithOneType();
    Forwarded fwd;
    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.typeRegistry = &reg;
    ctx.serverCommand = [&](std::string_view s) { fwd.lines.emplace_back(s); };
    registerConsoleCommands(cmds, ctx);

    CHECK(cmds.dispatch("spawn 99 1 2 3", systemIssuer()).find("unknown type") != std::string::npos);
    CHECK(cmds.dispatch("spawn nope:missing 1 2 3", systemIssuer()).find("unknown type") != std::string::npos);
    CHECK(fwd.lines.empty());
}

TEST_CASE("spawn: bad coordinates are refused before anything is queued (#1145)", "[console][commands]") {
    EntityTypeRegistry reg = registryWithOneType();
    Forwarded fwd;
    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.typeRegistry = &reg;
    ctx.serverCommand = [&](std::string_view s) { fwd.lines.emplace_back(s); };
    registerConsoleCommands(cmds, ctx);

    CHECK(cmds.dispatch("spawn test:unit x 2 3", systemIssuer()).find("invalid coordinates") != std::string::npos);
    CHECK(cmds.dispatch("spawn test:unit 1 y 3", systemIssuer()).find("invalid coordinates") != std::string::npos);
    CHECK(cmds.dispatch("spawn test:unit 1 2 z", systemIssuer()).find("invalid coordinates") != std::string::npos);
    CHECK(cmds.dispatch("spawn test:unit 1 2 3four", systemIssuer()).find("invalid coordinates") != std::string::npos);
    CHECK(fwd.lines.empty());

    // Negative and exponent forms are legitimate world coordinates and must survive.
    CHECK(cmds.dispatch("spawn test:unit -1000.5 1e3 +0", systemIssuer()).find("queued") != std::string::npos);
}

TEST_CASE("spawn: with no type registry the type is the server's problem (#1145)", "[console][commands]") {
    // A pure client that has not received the type table yet still has to be able to spawn; the
    // validation is best-effort, so its absence must not become a refusal.
    Forwarded fwd;
    CommandRegistry cmds;
    CommandContext ctx{}; // typeRegistry = nullptr
    ctx.serverCommand = [&](std::string_view s) { fwd.lines.emplace_back(s); };
    registerConsoleCommands(cmds, ctx);

    CHECK(cmds.dispatch("spawn anything:at:all 1 2 3", systemIssuer()).find("queued") != std::string::npos);
    CHECK(fwd.last() == "spawn anything:at:all 1 2 3");
}

// ---------------------------------------------------------------------------
// kill and tp
// ---------------------------------------------------------------------------

TEST_CASE("kill: the index must be a plain non-negative integer (#1145)", "[console][commands]") {
    Forwarded fwd;
    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.serverCommand = [&](std::string_view s) { fwd.lines.emplace_back(s); };
    registerConsoleCommands(cmds, ctx);

    CHECK(cmds.dispatch("kill -1", systemIssuer()).find("invalid entity index") != std::string::npos);
    CHECK(cmds.dispatch("kill 4.5", systemIssuer()).find("invalid entity index") != std::string::npos);
    CHECK(cmds.dispatch("kill abc", systemIssuer()).find("invalid entity index") != std::string::npos);
    CHECK(fwd.lines.empty());

    CHECK(cmds.dispatch("kill 7", systemIssuer()).find("queued") != std::string::npos);
    CHECK(fwd.last() == "kill 7");
}

TEST_CASE("tp: without a known player entity there is nothing to move (#1145)", "[console][commands]") {
    // The server needs to be told WHICH entity to teleport. Before the player's entity id arrives,
    // forwarding a bare `tp` would move whatever the server guessed.
    uint32_t idx = 3, gen = 1;
    Forwarded fwd;
    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.serverCommand = [&](std::string_view s) { fwd.lines.emplace_back(s); };
    registerConsoleCommands(cmds, ctx);
    CHECK(cmds.dispatch("tp 1 2 3", systemIssuer()).find("player entity unknown") != std::string::npos);

    CommandRegistry withGenOnly;
    CommandContext gctx = ctx;
    gctx.playerEntityGen = &gen; // index still missing
    registerConsoleCommands(withGenOnly, gctx);
    CHECK(withGenOnly.dispatch("tp 1 2 3", systemIssuer()).find("player entity unknown") != std::string::npos);
    CHECK(fwd.lines.empty());

    CommandRegistry full;
    CommandContext fctx = ctx;
    fctx.playerEntityIdx = &idx;
    fctx.playerEntityGen = &gen;
    registerConsoleCommands(full, fctx);
    CHECK(full.dispatch("tp 10 500 20", systemIssuer()).find("queued") != std::string::npos);
    CHECK(fwd.last() == "tp 3 10 500 20"); // the player's index leads the coordinates
}

TEST_CASE("tp: bad coordinates are refused (#1145)", "[console][commands]") {
    uint32_t idx = 3, gen = 1;
    Forwarded fwd;
    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.playerEntityIdx = &idx;
    ctx.playerEntityGen = &gen;
    ctx.serverCommand = [&](std::string_view s) { fwd.lines.emplace_back(s); };
    registerConsoleCommands(cmds, ctx);

    CHECK(cmds.dispatch("tp here 2 3", systemIssuer()).find("invalid coordinates") != std::string::npos);
    CHECK(cmds.dispatch("tp 1 there 3", systemIssuer()).find("invalid coordinates") != std::string::npos);
    CHECK(cmds.dispatch("tp 1 2 nowhere", systemIssuer()).find("invalid coordinates") != std::string::npos);
    CHECK(fwd.lines.empty());
}

// ---------------------------------------------------------------------------
// detonate
// ---------------------------------------------------------------------------

TEST_CASE("detonate: arguments are forwarded verbatim (#1145)", "[console][commands]") {
    // The server owns the world and the blast model, so the console deliberately does NOT parse
    // these — it must not develop a second opinion about what a valid blast radius is.
    Forwarded fwd;
    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.serverCommand = [&](std::string_view s) { fwd.lines.emplace_back(s); };
    registerConsoleCommands(cmds, ctx);

    CHECK(cmds.dispatch("detonate 0 100 0 250 5000", systemIssuer()).find("queued") != std::string::npos);
    CHECK(fwd.last() == "detonate 0 100 0 250 5000");

    CHECK(cmds.dispatch("detonate 0 100 0 2500 90000 --nuclear", systemIssuer()).find("queued") != std::string::npos);
    CHECK(fwd.last() == "detonate 0 100 0 2500 90000 --nuclear"); // the optional flag survives
}

TEST_CASE("detonate: too few arguments is a usage message, not a partial blast (#1145)", "[console][commands]") {
    Forwarded fwd;
    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.serverCommand = [&](std::string_view s) { fwd.lines.emplace_back(s); };
    registerConsoleCommands(cmds, ctx);

    CHECK(cmds.dispatch("detonate", systemIssuer()).find("usage") != std::string::npos);
    CHECK(cmds.dispatch("detonate 0 100 0 250", systemIssuer()).find("usage") != std::string::npos);
    CHECK(fwd.lines.empty());
}

// ---------------------------------------------------------------------------
// reload_content
// ---------------------------------------------------------------------------

TEST_CASE("reload_content: both halves run, and either alone still reports (#1145)", "[console][commands]") {
    // Content lives on both sides: the client caches meshes and textures, the server owns flight
    // models and Lua AI. A reload that only did one half would leave an author staring at a stale
    // asset and concluding hot reload does not work.
    SECTION("client and server") {
        Forwarded fwd;
        bool reloaded = false;
        CommandRegistry cmds;
        CommandContext ctx{};
        ctx.reloadContent = [&] {
            reloaded = true;
            return std::string("client: 12 assets evicted");
        };
        ctx.serverCommand = [&](std::string_view s) { fwd.lines.emplace_back(s); };
        registerConsoleCommands(cmds, ctx);

        const std::string out = cmds.dispatch("reload_content", systemIssuer());
        CHECK(reloaded);
        CHECK(fwd.last() == "reload_content");
        CHECK(out.find("12 assets evicted") != std::string::npos);
        CHECK(out.find("+ server") != std::string::npos);
    }
    SECTION("server only — a client with no renderer") {
        Forwarded fwd;
        CommandRegistry cmds;
        CommandContext ctx{};
        ctx.serverCommand = [&](std::string_view s) { fwd.lines.emplace_back(s); };
        registerConsoleCommands(cmds, ctx);

        CHECK(cmds.dispatch("reload_content", systemIssuer()).find("forwarded to server") != std::string::npos);
        CHECK(fwd.last() == "reload_content");
    }
    SECTION("client only — connected to a remote server") {
        CommandRegistry cmds;
        CommandContext ctx{};
        ctx.reloadContent = [] { return std::string("client: done"); };
        registerConsoleCommands(cmds, ctx);
        CHECK(cmds.dispatch("reload_content", systemIssuer()) == "client: done");
    }
    SECTION("neither") {
        CommandRegistry cmds;
        CommandContext ctx{};
        registerConsoleCommands(cmds, ctx);
        CHECK(cmds.dispatch("reload_content", systemIssuer()).find("not available") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// art (#841)
// ---------------------------------------------------------------------------

TEST_CASE("art: a channel is set by name and released by clear (#1145)", "[console][commands][art]") {
    struct Call {
        uint32_t idx;
        uint8_t channel;
        float value;
    };
    std::vector<Call> calls;
    int clears = 0;

    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.setArtChannel = [&](uint32_t i, uint8_t c, float v) { calls.push_back({i, c, v}); };
    ctx.clearArtChannels = [&] { ++clears; };
    registerConsoleCommands(cmds, ctx);

    CHECK(cmds.dispatch("art 3 gear 1.0", systemIssuer()).find("gear") != std::string::npos);
    REQUIRE(calls.size() == 1u);
    CHECK(calls[0].idx == 3u);
    CHECK(calls[0].channel == static_cast<uint8_t>(ArtChannel::Gear));
    CHECK(calls[0].value == 1.0f);

    // A signed channel takes a negative value — clamping it to 0 here would make it impossible to
    // scrub a control surface to one end of its travel.
    CHECK(cmds.dispatch("art 3 elevator -1", systemIssuer()).find("elevator") != std::string::npos);
    REQUIRE(calls.size() == 2u);
    CHECK(calls[1].value == -1.0f);

    CHECK(cmds.dispatch("art clear", systemIssuer()).find("cleared") != std::string::npos);
    CHECK(clears == 1);
    CHECK(calls.size() == 2u); // clear does not set anything
}

TEST_CASE("art: every channel name in the vocabulary resolves (#1145)", "[console][commands][art]") {
    // ArtChannel's enum order is the wire order and is ABI. If a name is added to the enum without a
    // string, this command silently reports it as unknown — and so would every content validator
    // that shares the table.
    std::vector<uint8_t> seen;
    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.setArtChannel = [&](uint32_t, uint8_t c, float) { seen.push_back(c); };
    ctx.clearArtChannels = [] {};
    registerConsoleCommands(cmds, ctx);

    for (std::size_t i = 0; i < kArtChannelCount; ++i) {
        const std::string name(artChannelName(static_cast<ArtChannel>(i)));
        INFO("channel " << name);
        CHECK(cmds.dispatch("art 1 " + name + " 0.5", systemIssuer()).find("unknown channel") == std::string::npos);
    }
    CHECK(seen.size() == kArtChannelCount);
}

TEST_CASE("art: an unknown channel lists the ones that exist (#1145)", "[console][commands][art]") {
    // The failure mode this replaces is an operator guessing names one at a time.
    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.setArtChannel = [](uint32_t, uint8_t, float) {};
    ctx.clearArtChannels = [] {};
    registerConsoleCommands(cmds, ctx);

    const std::string out = cmds.dispatch("art 1 wingtips 0.5", systemIssuer());
    CHECK(out.find("unknown channel") != std::string::npos);
    CHECK(out.find("gear") != std::string::npos); // the list is in the error itself
    CHECK(out.find("speedbrake") != std::string::npos);
}

TEST_CASE("art: malformed invocations do not reach the renderer (#1145)", "[console][commands][art]") {
    int sets = 0, clears = 0;
    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.setArtChannel = [&](uint32_t, uint8_t, float) { ++sets; };
    ctx.clearArtChannels = [&] { ++clears; };
    registerConsoleCommands(cmds, ctx);

    CHECK(cmds.dispatch("art", systemIssuer()).find("usage") != std::string::npos);
    CHECK(cmds.dispatch("art 1", systemIssuer()).find("usage") != std::string::npos);
    CHECK(cmds.dispatch("art 1 gear", systemIssuer()).find("usage") != std::string::npos);
    CHECK(cmds.dispatch("art 1 gear 0.5 extra", systemIssuer()).find("usage") != std::string::npos);
    CHECK(cmds.dispatch("art -1 gear 0.5", systemIssuer()).find("non-negative integer") != std::string::npos);
    CHECK(cmds.dispatch("art x gear 0.5", systemIssuer()).find("non-negative integer") != std::string::npos);
    CHECK(cmds.dispatch("art 1 gear open", systemIssuer()).find("must be a number") != std::string::npos);
    CHECK(sets == 0);
    CHECK(clears == 0);
}

TEST_CASE("art: without a renderer the command says so rather than doing nothing (#1145)", "[console][commands][art]") {
    // fl-server runs with a null renderer, and `art` is meaningless there.
    CommandRegistry cmds;
    CommandContext ctx{}; // both hooks null
    registerConsoleCommands(cmds, ctx);
    CHECK(cmds.dispatch("art 1 gear 1", systemIssuer()).find("not available") != std::string::npos);
    CHECK(cmds.dispatch("art clear", systemIssuer()).find("not available") != std::string::npos);

    // Half-wired is still unavailable: setting a channel with no way to release it would strand the
    // override on screen.
    CommandRegistry halfSet;
    CommandContext sctx{};
    sctx.setArtChannel = [](uint32_t, uint8_t, float) {};
    registerConsoleCommands(halfSet, sctx);
    CHECK(halfSet.dispatch("art 1 gear 1", systemIssuer()).find("not available") != std::string::npos);
}
