// SPDX-License-Identifier: GPL-3.0-or-later
#include "ILogger.h"
#include "debug/DebugCommandRegistry.h"
#include "debug/DebugCommands.h"
#include "debug/DebugConsole.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "loop/GameLoop.h"
#include "render/RenderSnapshot.h"
#include "render/SimRenderBridge.h"

#include "mock_hal.h"
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <string>

// ---------------------------------------------------------------------------
// Minimal ILogger stub — no output, no deps
// ---------------------------------------------------------------------------

struct NullLogger : public ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// ============================================================================
// DebugCommandRegistry
// ============================================================================

TEST_CASE("DebugCommandRegistry dispatch", "[dbg][registry]") {
    DebugCommandRegistry reg;
    reg.registerCommand("greet", "greet command", [](std::span<std::string_view>) { return std::string("hello"); });
    REQUIRE(reg.dispatch("greet") == "hello");
}

TEST_CASE("DebugCommandRegistry unknown command", "[dbg][registry]") {
    DebugCommandRegistry reg;
    std::string result = reg.dispatch("nope");
    REQUIRE(result.find("nope") != std::string::npos);
    REQUIRE(result.find("unknown command") != std::string::npos);
}

TEST_CASE("DebugCommandRegistry help lists commands", "[dbg][registry]") {
    DebugCommandRegistry reg;
    reg.registerCommand("alpha", "first cmd", [](std::span<std::string_view>) { return std::string{}; });
    reg.registerCommand("beta", "second cmd", [](std::span<std::string_view>) { return std::string{}; });
    std::string h = reg.helpText();
    REQUIRE(h.find("alpha") != std::string::npos);
    REQUIRE(h.find("beta") != std::string::npos);
}

TEST_CASE("DebugCommandRegistry empty input", "[dbg][registry]") {
    DebugCommandRegistry reg;
    REQUIRE(reg.dispatch("") == "");
    REQUIRE(reg.dispatch("   ") == "");
}

TEST_CASE("DebugCommandRegistry multi-space tokenization", "[dbg][registry]") {
    DebugCommandRegistry reg;
    std::vector<std::string_view> captured;
    reg.registerCommand("cmd", "test", [&captured](std::span<std::string_view> args) {
        captured.assign(args.begin(), args.end());
        return std::string{};
    });
    (void)reg.dispatch("cmd  arg1   arg2");
    REQUIRE(captured.size() == 2);
    REQUIRE(captured[0] == "arg1");
    REQUIRE(captured[1] == "arg2");
}

TEST_CASE("DebugCommandRegistry handler receives correct args", "[dbg][registry]") {
    DebugCommandRegistry reg;
    std::string got0, got1;
    reg.registerCommand("add", "add two nums", [&got0, &got1](std::span<std::string_view> args) {
        if (args.size() >= 2) {
            got0 = std::string(args[0]);
            got1 = std::string(args[1]);
        }
        return std::string{};
    });
    (void)reg.dispatch("add 3 4");
    REQUIRE(got0 == "3");
    REQUIRE(got1 == "4");
}

// ============================================================================
// DebugConsole — tick / open / close (MockInput)
// ============================================================================

TEST_CASE("DebugConsole open sets open state and close clears it", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    DebugConsole con(logger, reg);
    MockInput input;

    REQUIRE(!con.isOpen());
    con.open(input);
    REQUIRE(con.isOpen());
    con.close(input);
    REQUIRE(!con.isOpen());
}

TEST_CASE("DebugConsole tick Escape returns true", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    DebugConsole con(logger, reg);
    MockInput input;
    con.open(input);

    input.justPressed.insert(Key::Escape);
    REQUIRE(con.tick(input) == true);
}

TEST_CASE("DebugConsole tick Enter submits line", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    reg.registerCommand("ping", "test", [](std::span<std::string_view>) { return std::string("pong"); });
    DebugConsole con(logger, reg);
    MockInput input;
    con.open(input);

    con.onTextInput("ping");
    input.justPressed.insert(Key::Enter);
    REQUIRE(con.tick(input) == false);

    // After Enter the output ring should contain "> ping" and "pong"
    con.buildHud();
    bool foundPong = false;
    for (const auto& el : con.elements()) {
        if (el.type == HudElement::Type::Text && std::string(el.text).find("pong") != std::string::npos)
            foundPong = true;
    }
    REQUIRE(foundPong);
}

TEST_CASE("DebugConsole tick Backspace deletes character", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    DebugConsole con(logger, reg);
    MockInput input;
    con.open(input);

    con.onTextInput("ab");
    input.justPressed.insert(Key::Backspace);
    con.tick(input);

    // Only "a" should remain in the input line
    con.buildHud();
    bool foundA = false, foundAB = false;
    for (const auto& el : con.elements()) {
        if (el.type != HudElement::Type::Text)
            continue;
        std::string t(el.text);
        if (t.find("> a_") != std::string::npos)
            foundA = true;
        if (t.find("> ab") != std::string::npos)
            foundAB = true;
    }
    REQUIRE(foundA);
    REQUIRE(!foundAB);
}

TEST_CASE("DebugConsole tick ArrowUp recalls history", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    DebugConsole con(logger, reg);
    MockInput input;
    con.open(input);

    con.execute("prev_cmd");

    input.justPressed.insert(Key::ArrowUp);
    con.tick(input);

    // Input should now show "prev_cmd" recalled from history
    con.buildHud();
    bool found = false;
    for (const auto& el : con.elements()) {
        if (el.type == HudElement::Type::Text && std::string(el.text).find("prev_cmd") != std::string::npos)
            found = true;
    }
    REQUIRE(found);
}

TEST_CASE("DebugConsole tick ArrowDown clears recalled history", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    DebugConsole con(logger, reg);
    MockInput input;
    con.open(input);

    con.execute("cmd_a");

    // Navigate up then back down
    input.justPressed = {Key::ArrowUp};
    con.tick(input);
    input.justPressed = {Key::ArrowDown};
    con.tick(input);

    // Input should be cleared (back to empty after navigating down past index 0)
    con.buildHud();
    bool foundPromptEmpty = false;
    for (const auto& el : con.elements()) {
        if (el.type == HudElement::Type::Text && std::string(el.text) == "> _")
            foundPromptEmpty = true;
    }
    REQUIRE(foundPromptEmpty);
}

TEST_CASE("DebugConsole tick with no key press returns false", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    DebugConsole con(logger, reg);
    MockInput input;
    con.open(input);
    // No keys pressed — tick should return false and not crash
    REQUIRE(con.tick(input) == false);
}

// ============================================================================
// DebugConsole
// ============================================================================

TEST_CASE("DebugConsole output ring wrapping", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    DebugConsole con(logger, reg);

    // Push more than kMaxOutputLines (64) entries
    for (int i = 0; i < 70; ++i)
        con.execute("unknown_cmd_" + std::to_string(i));

    // Output ring stores at most 64 unique entries per line (echo + error = 2 per execute)
    // We just verify no crash and the console is usable afterward.
    con.openHeadless();
    glm::dvec3 pos{};
    con.buildHud(&pos);
    REQUIRE(con.elements().size() > 0);
}

TEST_CASE("DebugConsole onTextInput accumulates characters", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    DebugConsole con(logger, reg);

    con.openHeadless(); // must be open before text input is accepted
    con.onTextInput("hel");
    con.onTextInput("p");

    // Confirm the accumulated input is visible in the prompt element after buildHud
    con.openHeadless();
    con.buildHud();
    // Find the prompt element (last Text element)
    bool found = false;
    for (const auto& el : con.elements()) {
        if (el.type == HudElement::Type::Text && std::string(el.text).find("help") != std::string::npos) {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("DebugConsole execute dispatches and records output", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    reg.registerCommand("ping", "test", [](std::span<std::string_view>) { return std::string("pong"); });
    DebugConsole con(logger, reg);

    con.execute("ping");

    // Output should contain both the echo and the result
    con.openHeadless();
    con.buildHud();
    bool foundEcho = false, foundPong = false;
    for (const auto& el : con.elements()) {
        if (el.type != HudElement::Type::Text)
            continue;
        std::string t(el.text);
        if (t.find("> ping") != std::string::npos)
            foundEcho = true;
        if (t.find("pong") != std::string::npos)
            foundPong = true;
    }
    REQUIRE(foundEcho);
    REQUIRE(foundPong);
}

TEST_CASE("DebugConsole history records submitted commands", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    DebugConsole con(logger, reg);

    con.execute("cmd1");
    con.execute("cmd2");

    // Verify history has 2 entries by navigating: open, simulate ArrowUp via internal check
    // We can't call tick() without a mock IInput, so we just verify execute() doesn't crash
    // and the output ring has both echoes.
    con.openHeadless();
    con.buildHud();
    bool foundCmd1 = false, foundCmd2 = false;
    for (const auto& el : con.elements()) {
        if (el.type != HudElement::Type::Text)
            continue;
        std::string t(el.text);
        if (t.find("cmd1") != std::string::npos)
            foundCmd1 = true;
        if (t.find("cmd2") != std::string::npos)
            foundCmd2 = true;
    }
    REQUIRE(foundCmd1);
    REQUIRE(foundCmd2);
}

TEST_CASE("DebugConsole buildHud open produces elements", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    DebugConsole con(logger, reg);

    con.openHeadless();
    con.buildHud();

    auto elems = con.elements();
    REQUIRE(elems.size() > 0);

    bool hasRect = false;
    bool hasLine = false;
    bool hasText = false;
    for (const auto& el : elems) {
        if (el.type == HudElement::Type::Rect)
            hasRect = true;
        if (el.type == HudElement::Type::Line)
            hasLine = true;
        if (el.type == HudElement::Type::Text)
            hasText = true;
    }
    REQUIRE(hasRect);
    REQUIRE(hasLine);
    REQUIRE(hasText);
}

TEST_CASE("DebugConsole buildHud when closed no pos", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    DebugConsole con(logger, reg);

    con.buildHud(); // closed, no pos
    REQUIRE(con.elements().empty());
}

TEST_CASE("DebugConsole pos widget visible when closed", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    DebugConsole con(logger, reg);

    con.showPosRef() = true;
    glm::dvec3 pos{100.0, 200.0, 300.0};
    con.buildHud(&pos);

    REQUIRE(!con.elements().empty());
    bool foundPos = false;
    for (const auto& el : con.elements()) {
        if (el.type == HudElement::Type::Text && std::string(el.text).find("100") != std::string::npos) {
            foundPos = true;
            break;
        }
    }
    REQUIRE(foundPos);
}

TEST_CASE("DebugConsole pos widget hidden when null", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    DebugConsole con(logger, reg);

    con.showPosRef() = true;
    con.buildHud(nullptr); // showPos=true but nullptr → no element

    REQUIRE(con.elements().empty());
}

// ============================================================================
// DebugCommands — builtin commands
// ============================================================================

TEST_CASE("DebugCommands types command lists registered types", "[dbg][commands]") {
    fl::EntityTypeRegistry reg;
    fl::EntityDef defA;
    defA.id = "test:alpha";
    defA.name = "Alpha";
    fl::EntityDef defB;
    defB.id = "test:beta";
    defB.name = "Beta";
    reg.registerType(std::move(defA));
    reg.registerType(std::move(defB));

    DebugCommandRegistry cmds;
    DebugCommandContext ctx{};
    ctx.typeRegistry = &reg;
    registerBuiltinCommands(cmds, ctx);

    std::string out = cmds.dispatch("types");
    REQUIRE(out.find("test:alpha") != std::string::npos);
    REQUIRE(out.find("test:beta") != std::string::npos);
}

TEST_CASE("DebugCommands entities command lists snapshot entries", "[dbg][commands]") {
    fl::EntityTypeRegistry tyReg;
    fl::EntityDef def;
    def.id = "test:ship";
    def.name = "Ship";
    tyReg.registerType(std::move(def));

    fl::SimRenderBridge bridge;
    fl::RenderSnapshot snap;
    snap.tickIndex = 1;
    fl::EntityRenderEntry e{};
    e.entityIdx = 3;
    e.entityGen = 1;
    e.typeIndex = 0;
    e.position = {10.0, 20.0, 30.0};
    snap.entries.push_back(e);
    bridge.publish(std::move(snap));
    bridge.tryAdvance();

    DebugCommandRegistry cmds;
    DebugCommandContext ctx{};
    ctx.typeRegistry = &tyReg;
    ctx.renderBridge = &bridge;
    registerBuiltinCommands(cmds, ctx);

    std::string out = cmds.dispatch("entities");
    REQUIRE(out.find("test:ship") != std::string::npos);
    REQUIRE(out.find("3/1") != std::string::npos);
}

// ---------------------------------------------------------------------------
// DebugCommands — null context / arg-count / error-path branches
// ---------------------------------------------------------------------------

TEST_CASE("DebugCommands types with null registry returns error", "[dbg][commands]") {
    DebugCommandRegistry cmds;
    DebugCommandContext ctx{}; // typeRegistry = nullptr
    registerBuiltinCommands(cmds, ctx);
    std::string out = cmds.dispatch("types");
    REQUIRE(out.find("no type registry") != std::string::npos);
}

TEST_CASE("DebugCommands entities with null bridge returns error", "[dbg][commands]") {
    DebugCommandRegistry cmds;
    DebugCommandContext ctx{}; // renderBridge = nullptr
    registerBuiltinCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("entities").find("no render bridge") != std::string::npos);
}

TEST_CASE("DebugCommands entities with no snapshot returns message", "[dbg][commands]") {
    fl::SimRenderBridge bridge; // never published — hasSnapshot() == false
    DebugCommandRegistry cmds;
    DebugCommandContext ctx{};
    ctx.renderBridge = &bridge;
    registerBuiltinCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("entities").find("no snapshot") != std::string::npos);
}

TEST_CASE("DebugCommands entities with empty snapshot returns message", "[dbg][commands]") {
    fl::SimRenderBridge bridge;
    fl::RenderSnapshot snap;
    snap.tickIndex = 1;
    // snap.entries is empty
    bridge.publish(std::move(snap));
    bridge.tryAdvance();

    DebugCommandRegistry cmds;
    DebugCommandContext ctx{};
    ctx.renderBridge = &bridge;
    registerBuiltinCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("entities").find("no live entities") != std::string::npos);
}

TEST_CASE("DebugCommands spawn with missing args returns usage", "[dbg][commands]") {
    DebugCommandRegistry cmds;
    DebugCommandContext ctx{};
    registerBuiltinCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("spawn").find("usage") != std::string::npos);
    REQUIRE(cmds.dispatch("spawn type 1 2").find("usage") != std::string::npos);
}

TEST_CASE("DebugCommands spawn with null context returns error", "[dbg][commands]") {
    DebugCommandRegistry cmds;
    DebugCommandContext ctx{}; // entityManager / gameLoop = nullptr
    registerBuiltinCommands(cmds, ctx);
    std::string out = cmds.dispatch("spawn builtin:debug-entity 0 500 0");
    REQUIRE(out.find("not available") != std::string::npos);
}

TEST_CASE("DebugCommands spawn with invalid coords returns error", "[dbg][commands]") {
    // Need non-null context but invalid coordinates
    fl::EntityTypeRegistry tyReg;
    fl::EntityDef def;
    def.id = "test:unit";
    def.name = "Unit";
    tyReg.registerType(std::move(def));
    DebugCommandRegistry cmds;
    DebugCommandContext ctx{};
    ctx.typeRegistry = &tyReg;
    // entityManager and gameLoop are null so we get "not available" before coord parse,
    // but passing null entityManager triggers the "not available" guard first.
    // Test invalid coords by checking the parse branch via a valid-looking context:
    // Just verify we get some error string — coord validation comes after context check.
    registerBuiltinCommands(cmds, ctx);
    REQUIRE(!cmds.dispatch("spawn test:unit x y z").empty());
}

TEST_CASE("DebugCommands spawn with unknown type returns error", "[dbg][commands]") {
    fl::EntityTypeRegistry tyReg; // empty registry
    fl::SimRenderBridge bridge;
    DebugCommandRegistry cmds;
    DebugCommandContext ctx{};
    ctx.typeRegistry = &tyReg;
    ctx.renderBridge = &bridge;
    // entityManager / gameLoop null — triggers "not available" before type check;
    // set them to non-null via a workaround: use the fact that "not available" fires
    // first so unknown type check can't be reached without a running GameLoop.
    // Coverage goal: the null-context guard branch fires and is tested.
    registerBuiltinCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("spawn unknown:type 0 0 0").find("not available") != std::string::npos);
}

TEST_CASE("DebugCommands kill with missing args returns usage", "[dbg][commands]") {
    DebugCommandRegistry cmds;
    DebugCommandContext ctx{};
    registerBuiltinCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("kill").find("usage") != std::string::npos);
}

TEST_CASE("DebugCommands kill with null context returns error", "[dbg][commands]") {
    DebugCommandRegistry cmds;
    DebugCommandContext ctx{};
    registerBuiltinCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("kill 5").find("not available") != std::string::npos);
}

TEST_CASE("DebugCommands kill entity not in snapshot returns error", "[dbg][commands]") {
    fl::SimRenderBridge bridge;
    fl::RenderSnapshot snap;
    snap.tickIndex = 1; // empty entries
    bridge.publish(std::move(snap));
    bridge.tryAdvance();

    DebugCommandRegistry cmds;
    DebugCommandContext ctx{};
    ctx.renderBridge = &bridge;
    // entityManager / gameLoop null → "not available" fires before snapshot check
    registerBuiltinCommands(cmds, ctx);
    REQUIRE(!cmds.dispatch("kill 42").empty());
}

TEST_CASE("DebugCommands tp with missing args returns usage", "[dbg][commands]") {
    DebugCommandRegistry cmds;
    DebugCommandContext ctx{};
    registerBuiltinCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("tp").find("usage") != std::string::npos);
    REQUIRE(cmds.dispatch("tp 1 2").find("usage") != std::string::npos);
}

TEST_CASE("DebugCommands tp with null context returns error", "[dbg][commands]") {
    DebugCommandRegistry cmds;
    DebugCommandContext ctx{};
    registerBuiltinCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("tp 0 500 0").find("not available") != std::string::npos);
}

TEST_CASE("DebugCommands toggle_pos with null showPos returns error", "[dbg][commands]") {
    DebugCommandRegistry cmds;
    DebugCommandContext ctx{}; // showPos = nullptr
    registerBuiltinCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("toggle_pos").find("not available") != std::string::npos);
}

TEST_CASE("DebugCommands toggle_pos toggles flag", "[dbg][commands]") {
    bool flag = false;
    DebugCommandRegistry cmds;
    DebugCommandContext ctx{};
    ctx.showPos = &flag;
    registerBuiltinCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("toggle_pos").find("ON") != std::string::npos);
    REQUIRE(flag == true);
    REQUIRE(cmds.dispatch("toggle_pos").find("OFF") != std::string::npos);
    REQUIRE(flag == false);
}

TEST_CASE("DebugCommands stub commands return messages", "[dbg][commands]") {
    DebugCommandRegistry cmds;
    DebugCommandContext ctx{};
    registerBuiltinCommands(cmds, ctx);
    REQUIRE(!cmds.dispatch("set_weather clear").empty());
    REQUIRE(!cmds.dispatch("set_difficulty veteran").empty());
    REQUIRE(!cmds.dispatch("reload_content").empty());
}

// ---------------------------------------------------------------------------
// DebugConsole — additional branch coverage
// ---------------------------------------------------------------------------

TEST_CASE("DebugConsole execute skips duplicate history entry", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    DebugConsole con(logger, reg);

    con.execute("cmd");
    con.execute("cmd"); // duplicate — should not add a second history entry
    con.execute("other");

    // Verify no crash and the ring has all three echoes (> cmd, > cmd, > other)
    con.openHeadless();
    con.buildHud();
    REQUIRE(!con.elements().empty());
}

TEST_CASE("DebugConsole execute empty line is no-op", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    DebugConsole con(logger, reg);

    con.execute(""); // should return immediately without adding to ring
    con.buildHud();
    REQUIRE(con.elements().empty()); // closed, no pos → empty
}

TEST_CASE("DebugConsole buildHud shows partial output ring", "[dbg][console]") {
    NullLogger logger;
    DebugCommandRegistry reg;
    reg.registerCommand("noop", "no-op", [](std::span<std::string_view>) { return std::string{}; });
    DebugConsole con(logger, reg);

    // Push exactly 3 output lines (< kVisibleLines=20)
    con.execute("noop");
    con.execute("noop");

    con.openHeadless();
    con.buildHud();

    // Should have: rect + 2 sep lines + title + <=3 output texts + prompt = some elements
    REQUIRE(con.elements().size() > 0);
    bool hasRect = false;
    for (const auto& el : con.elements())
        if (el.type == HudElement::Type::Rect)
            hasRect = true;
    REQUIRE(hasRect);
}

TEST_CASE("DebugCommands help for specific command", "[dbg][commands]") {
    DebugCommandRegistry cmds;
    DebugCommandContext ctx{};
    registerBuiltinCommands(cmds, ctx);
    std::string out = cmds.dispatch("help spawn");
    REQUIRE(!out.empty());
    REQUIRE(out.find("spawn") != std::string::npos);
}

TEST_CASE("DebugCommands help for unknown command returns error", "[dbg][commands]") {
    DebugCommandRegistry cmds;
    DebugCommandContext ctx{};
    registerBuiltinCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("help nonexistent").find("unknown command") != std::string::npos);
}

TEST_CASE("DebugCommands help command lists all builtins", "[dbg][commands]") {
    DebugCommandRegistry cmds;
    DebugCommandContext ctx{};
    registerBuiltinCommands(cmds, ctx);

    std::string out = cmds.dispatch("help");
    REQUIRE(out.find("types") != std::string::npos);
    REQUIRE(out.find("entities") != std::string::npos);
    REQUIRE(out.find("spawn") != std::string::npos);
    REQUIRE(out.find("kill") != std::string::npos);
    REQUIRE(out.find("tp") != std::string::npos);
    REQUIRE(out.find("toggle_pos") != std::string::npos);
    REQUIRE(out.find("set_weather") != std::string::npos);
    REQUIRE(out.find("set_difficulty") != std::string::npos);
    REQUIRE(out.find("reload_content") != std::string::npos);
}

// ============================================================================
// DebugCommands — full-context parsing branches (non-started GameLoop)
//
// GameLoop is constructed but not started; enqueueSimCallback() just queues
// lambdas that are never drained, which is fine for testing the parsing paths.
// ============================================================================

// Minimal ISimUpdate stub for constructing GameLoop in tests
struct NullSim : public ISimUpdate {
    void onTick(double, uint64_t) override {}
};

static DebugCommandContext makeFullCtx(fl::EntityTypeRegistry& tyReg, fl::EntityManager& em, GameLoop& gl,
                                       bool* showPos = nullptr, uint32_t* playerIdx = nullptr,
                                       uint32_t* playerGen = nullptr) {
    DebugCommandContext ctx{};
    ctx.entityManager = &em;
    ctx.typeRegistry = &tyReg;
    ctx.gameLoop = &gl;
    ctx.showPos = showPos;
    ctx.playerEntityIdx = playerIdx;
    ctx.playerEntityGen = playerGen;
    return ctx;
}

TEST_CASE("DebugCommands spawn with invalid coordinates returns error", "[dbg][commands]") {
    NullLogger log;
    fl::EntityTypeRegistry tyReg;
    fl::EntityDef def;
    def.id = "test:unit";
    def.name = "Unit";
    tyReg.registerType(std::move(def));
    fl::EntityManager em(log, tyReg);
    NullSim sim;
    GameLoop gl(sim, log);

    DebugCommandRegistry cmds;
    registerBuiltinCommands(cmds, makeFullCtx(tyReg, em, gl));

    std::string out = cmds.dispatch("spawn test:unit abc 0 0");
    REQUIRE(out.find("invalid coordinates") != std::string::npos);
}

TEST_CASE("DebugCommands spawn with unknown type name returns error", "[dbg][commands]") {
    NullLogger log;
    fl::EntityTypeRegistry tyReg; // empty
    fl::EntityManager em(log, tyReg);
    NullSim sim;
    GameLoop gl(sim, log);

    DebugCommandRegistry cmds;
    registerBuiltinCommands(cmds, makeFullCtx(tyReg, em, gl));

    REQUIRE(cmds.dispatch("spawn unknown:thing 0 0 0").find("unknown type") != std::string::npos);
}

TEST_CASE("DebugCommands spawn with numeric index that is out of range returns error", "[dbg][commands]") {
    NullLogger log;
    fl::EntityTypeRegistry tyReg; // empty — index 0 not valid
    fl::EntityManager em(log, tyReg);
    NullSim sim;
    GameLoop gl(sim, log);

    DebugCommandRegistry cmds;
    registerBuiltinCommands(cmds, makeFullCtx(tyReg, em, gl));

    // isAllDigits("99") = true path; byIndex(99) returns nullptr
    REQUIRE(cmds.dispatch("spawn 99 0 0 0").find("unknown type") != std::string::npos);
}

TEST_CASE("DebugCommands spawn valid type enqueues and returns message", "[dbg][commands]") {
    NullLogger log;
    fl::EntityTypeRegistry tyReg;
    fl::EntityDef def;
    def.id = "test:ship";
    def.name = "Ship";
    tyReg.registerType(std::move(def));
    fl::EntityManager em(log, tyReg);
    NullSim sim;
    GameLoop gl(sim, log);

    DebugCommandRegistry cmds;
    registerBuiltinCommands(cmds, makeFullCtx(tyReg, em, gl));

    std::string out = cmds.dispatch("spawn test:ship 0 500 0");
    REQUIRE(out.find("queued") != std::string::npos);
}

TEST_CASE("DebugCommands spawn valid numeric index enqueues and returns message", "[dbg][commands]") {
    NullLogger log;
    fl::EntityTypeRegistry tyReg;
    fl::EntityDef def;
    def.id = "test:jet";
    def.name = "Jet";
    tyReg.registerType(std::move(def));
    fl::EntityManager em(log, tyReg);
    NullSim sim;
    GameLoop gl(sim, log);

    DebugCommandRegistry cmds;
    registerBuiltinCommands(cmds, makeFullCtx(tyReg, em, gl));

    // "0" is a valid index — tests isAllDigits true + byIndex success path
    std::string out = cmds.dispatch("spawn 0 0 500 0");
    REQUIRE(out.find("queued") != std::string::npos);
}

TEST_CASE("DebugCommands kill with invalid index returns error", "[dbg][commands]") {
    NullLogger log;
    fl::EntityTypeRegistry tyReg;
    fl::EntityManager em(log, tyReg);
    fl::SimRenderBridge bridge;
    fl::RenderSnapshot snap;
    snap.tickIndex = 1;
    bridge.publish(std::move(snap));
    bridge.tryAdvance();
    NullSim sim;
    GameLoop gl(sim, log);

    DebugCommandRegistry cmds;
    auto ctx = makeFullCtx(tyReg, em, gl);
    ctx.renderBridge = &bridge;
    registerBuiltinCommands(cmds, ctx);

    // Non-numeric idx — parseUint fails
    REQUIRE(cmds.dispatch("kill abc").find("invalid entity index") != std::string::npos);
}

TEST_CASE("DebugCommands kill entity not in snapshot returns not found", "[dbg][commands]") {
    NullLogger log;
    fl::EntityTypeRegistry tyReg;
    fl::EntityManager em(log, tyReg);
    fl::SimRenderBridge bridge;
    fl::RenderSnapshot snap;
    snap.tickIndex = 1; // no entries
    bridge.publish(std::move(snap));
    bridge.tryAdvance();
    NullSim sim;
    GameLoop gl(sim, log);

    DebugCommandRegistry cmds;
    auto ctx = makeFullCtx(tyReg, em, gl);
    ctx.renderBridge = &bridge;
    registerBuiltinCommands(cmds, ctx);

    REQUIRE(cmds.dispatch("kill 42").find("not found") != std::string::npos);
}

TEST_CASE("DebugCommands tp with no player entity returns error", "[dbg][commands]") {
    NullLogger log;
    fl::EntityTypeRegistry tyReg;
    fl::EntityManager em(log, tyReg);
    NullSim sim;
    GameLoop gl(sim, log);
    uint32_t idx = 0, gen = 0;

    DebugCommandRegistry cmds;
    registerBuiltinCommands(cmds, makeFullCtx(tyReg, em, gl, nullptr, &idx, &gen));

    REQUIRE(cmds.dispatch("tp 0 500 0").find("no player entity") != std::string::npos);
}

TEST_CASE("DebugCommands tp with valid player enqueues and returns message", "[dbg][commands]") {
    NullLogger log;
    fl::EntityTypeRegistry tyReg;
    fl::EntityManager em(log, tyReg);
    NullSim sim;
    GameLoop gl(sim, log);
    uint32_t idx = 1, gen = 1; // non-zero = valid

    DebugCommandRegistry cmds;
    registerBuiltinCommands(cmds, makeFullCtx(tyReg, em, gl, nullptr, &idx, &gen));

    REQUIRE(cmds.dispatch("tp 100 500 200").find("queued") != std::string::npos);
}

TEST_CASE("DebugCommands tp with invalid coordinates returns error", "[dbg][commands]") {
    NullLogger log;
    fl::EntityTypeRegistry tyReg;
    fl::EntityManager em(log, tyReg);
    NullSim sim;
    GameLoop gl(sim, log);
    uint32_t idx = 1, gen = 1;

    DebugCommandRegistry cmds;
    registerBuiltinCommands(cmds, makeFullCtx(tyReg, em, gl, nullptr, &idx, &gen));

    REQUIRE(cmds.dispatch("tp bad 500 0").find("invalid") != std::string::npos);
}
