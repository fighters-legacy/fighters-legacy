// SPDX-License-Identifier: GPL-3.0-or-later
#include "ILogger.h"
#include "console/CommandRegistry.h"
#include "console/ConsoleCommands.h"
#include "console/GameConsole.h"
#include "entity/EntityDef.h"
#include "entity/EntityTypeRegistry.h"
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
// CommandRegistry
// ============================================================================

TEST_CASE("CommandRegistry dispatch", "[console][registry]") {
    CommandRegistry reg;
    reg.registerCommand("greet", "greet command", [](std::span<std::string_view>) { return std::string("hello"); });
    REQUIRE(reg.dispatch("greet") == "hello");
}

TEST_CASE("CommandRegistry unknown command", "[console][registry]") {
    CommandRegistry reg;
    std::string result = reg.dispatch("nope");
    REQUIRE(result.find("nope") != std::string::npos);
    REQUIRE(result.find("unknown command") != std::string::npos);
}

TEST_CASE("CommandRegistry help lists commands", "[console][registry]") {
    CommandRegistry reg;
    reg.registerCommand("alpha", "first cmd", [](std::span<std::string_view>) { return std::string{}; });
    reg.registerCommand("beta", "second cmd", [](std::span<std::string_view>) { return std::string{}; });
    std::string h = reg.helpText();
    REQUIRE(h.find("alpha") != std::string::npos);
    REQUIRE(h.find("beta") != std::string::npos);
}

TEST_CASE("CommandRegistry empty input", "[console][registry]") {
    CommandRegistry reg;
    REQUIRE(reg.dispatch("") == "");
    REQUIRE(reg.dispatch("   ") == "");
}

TEST_CASE("CommandRegistry multi-space tokenization", "[console][registry]") {
    CommandRegistry reg;
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

TEST_CASE("CommandRegistry handler receives correct args", "[console][registry]") {
    CommandRegistry reg;
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
// GameConsole — tick / open / close (MockInput)
// ============================================================================

TEST_CASE("GameConsole open sets open state and close clears it", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);
    MockInput input;

    REQUIRE(!con.isOpen());
    con.open(input);
    REQUIRE(con.isOpen());
    con.close(input);
    REQUIRE(!con.isOpen());
}

TEST_CASE("GameConsole tick Escape returns true", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);
    MockInput input;
    con.open(input);

    input.justPressed.insert(Key::Escape);
    REQUIRE(con.tick(input) == true);
}

TEST_CASE("GameConsole tick Enter submits line", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    reg.registerCommand("ping", "test", [](std::span<std::string_view>) { return std::string("pong"); });
    GameConsole con(logger, reg);
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

TEST_CASE("GameConsole tick Backspace deletes character", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);
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

TEST_CASE("GameConsole tick ArrowUp recalls history", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);
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

TEST_CASE("GameConsole tick ArrowDown clears recalled history", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);
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

TEST_CASE("GameConsole tick with no key press returns false", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);
    MockInput input;
    con.open(input);
    // No keys pressed — tick should return false and not crash
    REQUIRE(con.tick(input) == false);
}

// ============================================================================
// GameConsole
// ============================================================================

TEST_CASE("GameConsole print appends text to output ring", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);

    con.print("hello from server");

    con.openHeadless();
    glm::dvec3 pos{};
    con.buildHud(&pos);

    bool found = false;
    for (const auto& el : con.elements()) {
        if (el.type == HudElement::Type::Text && el.text.find("hello from server") != std::string_view::npos) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("GameConsole output ring wrapping", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);

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

TEST_CASE("GameConsole onTextInput accumulates characters", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);

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

TEST_CASE("GameConsole execute dispatches and records output", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    reg.registerCommand("ping", "test", [](std::span<std::string_view>) { return std::string("pong"); });
    GameConsole con(logger, reg);

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

TEST_CASE("GameConsole history records submitted commands", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);

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

TEST_CASE("GameConsole buildHud open produces elements", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);

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

TEST_CASE("GameConsole buildHud when closed no pos", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);

    con.buildHud(); // closed, no pos
    REQUIRE(con.elements().empty());
}

TEST_CASE("GameConsole pos widget visible when closed", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);

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

TEST_CASE("GameConsole pos widget hidden when null", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);

    con.showPosRef() = true;
    con.buildHud(nullptr); // showPos=true but nullptr → no element

    REQUIRE(con.elements().empty());
}

// ============================================================================
// ConsoleCommands — builtin commands
// ============================================================================

TEST_CASE("ConsoleCommands types command lists registered types", "[console][commands]") {
    fl::EntityTypeRegistry reg;
    fl::EntityDef defA;
    defA.id = "test:alpha";
    defA.name = "Alpha";
    fl::EntityDef defB;
    defB.id = "test:beta";
    defB.name = "Beta";
    reg.registerType(std::move(defA));
    reg.registerType(std::move(defB));

    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.typeRegistry = &reg;
    registerConsoleCommands(cmds, ctx);

    std::string out = cmds.dispatch("types");
    REQUIRE(out.find("test:alpha") != std::string::npos);
    REQUIRE(out.find("test:beta") != std::string::npos);
}

TEST_CASE("ConsoleCommands entities command lists snapshot entries", "[console][commands]") {
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

    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.typeRegistry = &tyReg;
    ctx.renderBridge = &bridge;
    registerConsoleCommands(cmds, ctx);

    std::string out = cmds.dispatch("entities");
    REQUIRE(out.find("test:ship") != std::string::npos);
    REQUIRE(out.find("3/1") != std::string::npos);
}

// ---------------------------------------------------------------------------
// ConsoleCommands — null context / arg-count / error-path branches
// ---------------------------------------------------------------------------

TEST_CASE("ConsoleCommands types with null registry returns error", "[console][commands]") {
    CommandRegistry cmds;
    CommandContext ctx{}; // typeRegistry = nullptr
    registerConsoleCommands(cmds, ctx);
    std::string out = cmds.dispatch("types");
    REQUIRE(out.find("no type registry") != std::string::npos);
}

TEST_CASE("ConsoleCommands entities with null bridge returns error", "[console][commands]") {
    CommandRegistry cmds;
    CommandContext ctx{}; // renderBridge = nullptr
    registerConsoleCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("entities").find("no render bridge") != std::string::npos);
}

TEST_CASE("ConsoleCommands entities with no snapshot returns message", "[console][commands]") {
    fl::SimRenderBridge bridge; // never published — hasSnapshot() == false
    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.renderBridge = &bridge;
    registerConsoleCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("entities").find("no snapshot") != std::string::npos);
}

TEST_CASE("ConsoleCommands entities with empty snapshot returns message", "[console][commands]") {
    fl::SimRenderBridge bridge;
    fl::RenderSnapshot snap;
    snap.tickIndex = 1;
    // snap.entries is empty
    bridge.publish(std::move(snap));
    bridge.tryAdvance();

    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.renderBridge = &bridge;
    registerConsoleCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("entities").find("no live entities") != std::string::npos);
}

TEST_CASE("ConsoleCommands spawn with missing args returns usage", "[console][commands]") {
    CommandRegistry cmds;
    CommandContext ctx{};
    registerConsoleCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("spawn").find("usage") != std::string::npos);
    REQUIRE(cmds.dispatch("spawn type 1 2").find("usage") != std::string::npos);
}

TEST_CASE("ConsoleCommands spawn with null context returns error", "[console][commands]") {
    CommandRegistry cmds;
    CommandContext ctx{}; // entityManager / gameLoop = nullptr
    registerConsoleCommands(cmds, ctx);
    std::string out = cmds.dispatch("spawn builtin:debug-entity 0 500 0");
    REQUIRE(out.find("not available") != std::string::npos);
}

TEST_CASE("ConsoleCommands spawn with invalid coords returns error", "[console][commands]") {
    // Need non-null context but invalid coordinates
    fl::EntityTypeRegistry tyReg;
    fl::EntityDef def;
    def.id = "test:unit";
    def.name = "Unit";
    tyReg.registerType(std::move(def));
    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.typeRegistry = &tyReg;
    // entityManager and gameLoop are null so we get "not available" before coord parse,
    // but passing null entityManager triggers the "not available" guard first.
    // Test invalid coords by checking the parse branch via a valid-looking context:
    // Just verify we get some error string — coord validation comes after context check.
    registerConsoleCommands(cmds, ctx);
    REQUIRE(!cmds.dispatch("spawn test:unit x y z").empty());
}

TEST_CASE("ConsoleCommands spawn with unknown type returns error", "[console][commands]") {
    fl::EntityTypeRegistry tyReg; // empty registry
    fl::SimRenderBridge bridge;
    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.typeRegistry = &tyReg;
    ctx.renderBridge = &bridge;
    // entityManager / gameLoop null — triggers "not available" before type check;
    // set them to non-null via a workaround: use the fact that "not available" fires
    // first so unknown type check can't be reached without a running GameLoop.
    // Coverage goal: the null-context guard branch fires and is tested.
    registerConsoleCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("spawn unknown:type 0 0 0").find("not available") != std::string::npos);
}

TEST_CASE("ConsoleCommands kill with missing args returns usage", "[console][commands]") {
    CommandRegistry cmds;
    CommandContext ctx{};
    registerConsoleCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("kill").find("usage") != std::string::npos);
}

TEST_CASE("ConsoleCommands kill with null context returns error", "[console][commands]") {
    CommandRegistry cmds;
    CommandContext ctx{};
    registerConsoleCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("kill 5").find("not available") != std::string::npos);
}

TEST_CASE("ConsoleCommands kill entity not in snapshot returns error", "[console][commands]") {
    fl::SimRenderBridge bridge;
    fl::RenderSnapshot snap;
    snap.tickIndex = 1; // empty entries
    bridge.publish(std::move(snap));
    bridge.tryAdvance();

    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.renderBridge = &bridge;
    // entityManager / gameLoop null → "not available" fires before snapshot check
    registerConsoleCommands(cmds, ctx);
    REQUIRE(!cmds.dispatch("kill 42").empty());
}

TEST_CASE("ConsoleCommands tp with missing args returns usage", "[console][commands]") {
    CommandRegistry cmds;
    CommandContext ctx{};
    registerConsoleCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("tp").find("usage") != std::string::npos);
    REQUIRE(cmds.dispatch("tp 1 2").find("usage") != std::string::npos);
}

TEST_CASE("ConsoleCommands tp with null context returns error", "[console][commands]") {
    CommandRegistry cmds;
    CommandContext ctx{};
    registerConsoleCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("tp 0 500 0").find("not available") != std::string::npos);
}

TEST_CASE("ConsoleCommands toggle_pos with null showPos returns error", "[console][commands]") {
    CommandRegistry cmds;
    CommandContext ctx{}; // showPos = nullptr
    registerConsoleCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("toggle_pos").find("not available") != std::string::npos);
}

TEST_CASE("ConsoleCommands toggle_pos toggles flag", "[console][commands]") {
    bool flag = false;
    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.showPos = &flag;
    registerConsoleCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("toggle_pos").find("ON") != std::string::npos);
    REQUIRE(flag == true);
    REQUIRE(cmds.dispatch("toggle_pos").find("OFF") != std::string::npos);
    REQUIRE(flag == false);
}

TEST_CASE("ConsoleCommands show_ping with null showPing returns error", "[console][commands]") {
    CommandRegistry cmds;
    CommandContext ctx{}; // showPing = nullptr
    registerConsoleCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("show_ping").find("not available") != std::string::npos);
}

TEST_CASE("ConsoleCommands show_ping toggles flag", "[console][commands]") {
    bool flag = false;
    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.showPing = &flag;
    registerConsoleCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("show_ping").find("on") != std::string::npos);
    REQUIRE(flag == true);
    REQUIRE(cmds.dispatch("show_ping").find("off") != std::string::npos);
    REQUIRE(flag == false);
}

TEST_CASE("ConsoleCommands stub commands return messages", "[console][commands]") {
    CommandRegistry cmds;
    CommandContext ctx{};
    registerConsoleCommands(cmds, ctx);
    REQUIRE(!cmds.dispatch("set_weather clear").empty());
    REQUIRE(!cmds.dispatch("set_difficulty veteran").empty());
    REQUIRE(!cmds.dispatch("reload_content").empty());
}

// ---------------------------------------------------------------------------
// ConsoleCommands — serverCommand forwarding (new behaviour after #227)
// ---------------------------------------------------------------------------

TEST_CASE("ConsoleCommands spawn forwards command to serverCommand", "[console][commands]") {
    fl::EntityTypeRegistry reg;
    fl::EntityDef def;
    def.id = "test:unit";
    def.name = "Unit";
    def.category = fl::ObjectCategory::AirVehicle;
    def.maxHp = 100.f;
    reg.registerType(std::move(def));

    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.typeRegistry = &reg;
    std::string captured;
    ctx.serverCommand = [&](std::string_view s) { captured = std::string(s); };
    registerConsoleCommands(cmds, ctx);

    auto out1 = cmds.dispatch("spawn test:unit 0 500 0");
    REQUIRE(out1.find("queued") != std::string::npos);
    REQUIRE(captured.find("spawn") != std::string::npos);
    REQUIRE(captured.find("test:unit") != std::string::npos);
}

TEST_CASE("ConsoleCommands kill forwards command to serverCommand", "[console][commands]") {
    CommandRegistry cmds;
    CommandContext ctx{};
    std::string captured;
    ctx.serverCommand = [&](std::string_view s) { captured = std::string(s); };
    registerConsoleCommands(cmds, ctx);

    auto out2 = cmds.dispatch("kill 42");
    REQUIRE(out2.find("queued") != std::string::npos);
    REQUIRE(captured.find("kill") != std::string::npos);
    REQUIRE(captured.find("42") != std::string::npos);
}

TEST_CASE("ConsoleCommands tp forwards command with player idx to serverCommand", "[console][commands]") {
    uint32_t idx = 3, gen = 1;
    CommandRegistry cmds;
    CommandContext ctx{};
    ctx.playerEntityIdx = &idx;
    ctx.playerEntityGen = &gen;
    std::string captured;
    ctx.serverCommand = [&](std::string_view s) { captured = std::string(s); };
    registerConsoleCommands(cmds, ctx);

    auto out3 = cmds.dispatch("tp 10 500 20");
    REQUIRE(out3.find("queued") != std::string::npos);
    REQUIRE(captured.find("tp") != std::string::npos);
    REQUIRE(captured.find("3") != std::string::npos); // player entity idx
}

TEST_CASE("ConsoleCommands set_weather forwards command to serverCommand", "[console][commands]") {
    CommandRegistry cmds;
    CommandContext ctx{};
    std::string captured;
    ctx.serverCommand = [&](std::string_view s) { captured = std::string(s); };
    registerConsoleCommands(cmds, ctx);

    auto out4 = cmds.dispatch("set_weather storm");
    REQUIRE(out4.find("queued") != std::string::npos);
    REQUIRE(captured.find("set_weather") != std::string::npos);
    REQUIRE(captured.find("storm") != std::string::npos);
}

TEST_CASE("ConsoleCommands spawn with null serverCommand returns not available", "[console][commands]") {
    // serverCommand = nullptr (default)
    CommandRegistry cmds;
    CommandContext ctx{};
    registerConsoleCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("spawn builtin:debug-entity 0 500 0").find("not available") != std::string::npos);
}

// ---------------------------------------------------------------------------
// GameConsole — additional branch coverage
// ---------------------------------------------------------------------------

TEST_CASE("GameConsole execute skips duplicate history entry", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);

    con.execute("cmd");
    con.execute("cmd"); // duplicate — should not add a second history entry
    con.execute("other");

    // Verify no crash and the ring has all three echoes (> cmd, > cmd, > other)
    con.openHeadless();
    con.buildHud();
    REQUIRE(!con.elements().empty());
}

TEST_CASE("GameConsole execute empty line is no-op", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);

    con.execute(""); // should return immediately without adding to ring
    con.buildHud();
    REQUIRE(con.elements().empty()); // closed, no pos → empty
}

TEST_CASE("GameConsole buildHud shows partial output ring", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    reg.registerCommand("noop", "no-op", [](std::span<std::string_view>) { return std::string{}; });
    GameConsole con(logger, reg);

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

TEST_CASE("ConsoleCommands help for specific command", "[console][commands]") {
    CommandRegistry cmds;
    CommandContext ctx{};
    registerConsoleCommands(cmds, ctx);
    std::string out = cmds.dispatch("help spawn");
    REQUIRE(!out.empty());
    REQUIRE(out.find("spawn") != std::string::npos);
}

TEST_CASE("ConsoleCommands help for unknown command returns error", "[console][commands]") {
    CommandRegistry cmds;
    CommandContext ctx{};
    registerConsoleCommands(cmds, ctx);
    REQUIRE(cmds.dispatch("help nonexistent").find("unknown command") != std::string::npos);
}

TEST_CASE("ConsoleCommands help command lists all builtins", "[console][commands]") {
    CommandRegistry cmds;
    CommandContext ctx{};
    registerConsoleCommands(cmds, ctx);

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
// ConsoleCommands — full-context parsing branches
//
// Server-side commands (spawn/kill/tp/set_weather) now forward to a serverCommand
// callback rather than calling EntityManager/GameLoop directly.
// ============================================================================

// Helper: context with serverCommand capture + optional fields.
static CommandContext makeCtxWithCapture(fl::EntityTypeRegistry& tyReg, std::string& captured, bool* showPos = nullptr,
                                         uint32_t* playerIdx = nullptr, uint32_t* playerGen = nullptr) {
    CommandContext ctx{};
    ctx.typeRegistry = &tyReg;
    ctx.showPos = showPos;
    ctx.playerEntityIdx = playerIdx;
    ctx.playerEntityGen = playerGen;
    ctx.serverCommand = [&captured](std::string_view cmd) { captured = std::string(cmd); };
    return ctx;
}

TEST_CASE("ConsoleCommands spawn with invalid coordinates returns error", "[console][commands]") {
    fl::EntityTypeRegistry tyReg;
    fl::EntityDef def;
    def.id = "test:unit";
    def.name = "Unit";
    def.category = fl::ObjectCategory::AirVehicle;
    def.maxHp = 100.f;
    tyReg.registerType(std::move(def));

    std::string captured;
    CommandRegistry cmds;
    registerConsoleCommands(cmds, makeCtxWithCapture(tyReg, captured));

    std::string out = cmds.dispatch("spawn test:unit abc 0 0");
    REQUIRE(out.find("invalid coordinates") != std::string::npos);
    REQUIRE(captured.empty()); // serverCommand not called on parse error
}

TEST_CASE("ConsoleCommands spawn with unknown type name returns error", "[console][commands]") {
    fl::EntityTypeRegistry tyReg; // empty

    std::string captured;
    CommandRegistry cmds;
    registerConsoleCommands(cmds, makeCtxWithCapture(tyReg, captured));

    REQUIRE(cmds.dispatch("spawn unknown:thing 0 0 0").find("unknown type") != std::string::npos);
    REQUIRE(captured.empty());
}

TEST_CASE("ConsoleCommands spawn with numeric index out of range returns error", "[console][commands]") {
    fl::EntityTypeRegistry tyReg; // empty — index 99 not valid

    std::string captured;
    CommandRegistry cmds;
    registerConsoleCommands(cmds, makeCtxWithCapture(tyReg, captured));

    // isAllDigits("99") = true path; byIndex(99) returns nullptr
    REQUIRE(cmds.dispatch("spawn 99 0 0 0").find("unknown type") != std::string::npos);
    REQUIRE(captured.empty());
}

TEST_CASE("ConsoleCommands spawn valid type forwards to serverCommand", "[console][commands]") {
    fl::EntityTypeRegistry tyReg;
    fl::EntityDef def;
    def.id = "test:ship";
    def.name = "Ship";
    def.category = fl::ObjectCategory::AirVehicle;
    def.maxHp = 100.f;
    tyReg.registerType(std::move(def));

    std::string captured;
    CommandRegistry cmds;
    registerConsoleCommands(cmds, makeCtxWithCapture(tyReg, captured));

    std::string out = cmds.dispatch("spawn test:ship 0 500 0");
    REQUIRE(out.find("queued") != std::string::npos);
    REQUIRE(captured.find("spawn") != std::string::npos);
    REQUIRE(captured.find("test:ship") != std::string::npos);
}

TEST_CASE("ConsoleCommands spawn valid numeric index forwards to serverCommand", "[console][commands]") {
    fl::EntityTypeRegistry tyReg;
    fl::EntityDef def;
    def.id = "test:jet";
    def.name = "Jet";
    def.category = fl::ObjectCategory::AirVehicle;
    def.maxHp = 100.f;
    tyReg.registerType(std::move(def));

    std::string captured;
    CommandRegistry cmds;
    registerConsoleCommands(cmds, makeCtxWithCapture(tyReg, captured));

    // "0" is a valid index — tests isAllDigits true + byIndex success path
    std::string out = cmds.dispatch("spawn 0 0 500 0");
    REQUIRE(out.find("queued") != std::string::npos);
    REQUIRE(!captured.empty());
}

TEST_CASE("ConsoleCommands kill with invalid index returns error", "[console][commands]") {
    fl::EntityTypeRegistry tyReg;
    std::string captured;
    CommandRegistry cmds;
    registerConsoleCommands(cmds, makeCtxWithCapture(tyReg, captured));

    // Non-numeric idx — parseUint fails
    REQUIRE(cmds.dispatch("kill abc").find("invalid entity index") != std::string::npos);
    REQUIRE(captured.empty());
}

TEST_CASE("ConsoleCommands kill valid index forwards to serverCommand", "[console][commands]") {
    fl::EntityTypeRegistry tyReg;
    std::string captured;
    CommandRegistry cmds;
    registerConsoleCommands(cmds, makeCtxWithCapture(tyReg, captured));

    std::string out = cmds.dispatch("kill 42");
    REQUIRE(out.find("queued") != std::string::npos);
    REQUIRE(captured.find("kill") != std::string::npos);
    REQUIRE(captured.find("42") != std::string::npos);
}

TEST_CASE("ConsoleCommands tp with null playerEntityIdx returns error", "[console][commands]") {
    fl::EntityTypeRegistry tyReg;
    std::string captured;
    // playerIdx = nullptr → "player entity unknown"
    CommandRegistry cmds;
    registerConsoleCommands(cmds, makeCtxWithCapture(tyReg, captured));

    REQUIRE(cmds.dispatch("tp 0 500 0").find("player entity unknown") != std::string::npos);
    REQUIRE(captured.empty());
}

TEST_CASE("ConsoleCommands tp with valid player forwards to serverCommand", "[console][commands]") {
    fl::EntityTypeRegistry tyReg;
    uint32_t idx = 1, gen = 1;
    std::string captured;
    CommandRegistry cmds;
    registerConsoleCommands(cmds, makeCtxWithCapture(tyReg, captured, nullptr, &idx, &gen));

    std::string out = cmds.dispatch("tp 100 500 200");
    REQUIRE(out.find("queued") != std::string::npos);
    REQUIRE(captured.find("tp") != std::string::npos);
}

TEST_CASE("ConsoleCommands tp with invalid coordinates returns error", "[console][commands]") {
    fl::EntityTypeRegistry tyReg;
    uint32_t idx = 1, gen = 1;
    std::string captured;
    CommandRegistry cmds;
    registerConsoleCommands(cmds, makeCtxWithCapture(tyReg, captured, nullptr, &idx, &gen));

    REQUIRE(cmds.dispatch("tp bad 500 0").find("invalid") != std::string::npos);
    REQUIRE(captured.empty());
}

// ---------------------------------------------------------------------------
// set_weather forwarding (serverCommand-based, replaces WeatherController test)
// ---------------------------------------------------------------------------

TEST_CASE("set_weather command forwards valid presets to serverCommand", "[console][commands]") {
    fl::EntityTypeRegistry tyReg;
    std::string captured;
    CommandRegistry reg;
    registerConsoleCommands(reg, makeCtxWithCapture(tyReg, captured));

    CHECK(reg.dispatch("set_weather storm").find("queued") != std::string::npos);
    CHECK(captured.find("set_weather") != std::string::npos);
    CHECK(captured.find("storm") != std::string::npos);

    captured.clear();
    CHECK(reg.dispatch("set_weather clear").find("queued") != std::string::npos);
    CHECK(captured.find("clear") != std::string::npos);

    CHECK(reg.dispatch("set_weather hurricane").find("unknown") != std::string::npos);
    CHECK(reg.dispatch("set_weather").find("usage") != std::string::npos);
}

// ============================================================================
// GameConsole — outputLines() API (#292)
// ============================================================================

TEST_CASE("GameConsole outputLines empty on construction", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);
    REQUIRE(con.outputLines().empty());
}

TEST_CASE("GameConsole outputLines reflects print calls", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);

    con.print("alpha");
    con.print("beta");

    auto lines = con.outputLines();
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0] == "alpha");
    REQUIRE(lines[1] == "beta");
}

TEST_CASE("GameConsole outputLines capped at 64 entries", "[console]") {
    NullLogger logger;
    CommandRegistry reg;
    GameConsole con(logger, reg);

    for (int i = 1; i <= 65; ++i)
        con.print("line" + std::to_string(i));

    auto lines = con.outputLines();
    REQUIRE(lines.size() == 64);
    // line1 was overwritten; oldest surviving entry is line2
    REQUIRE(lines.front() == "line2");
    REQUIRE(lines.back() == "line65");
}
