// SPDX-License-Identifier: GPL-3.0-or-later
#include "debug/DebugCommands.h"

#include "debug/DebugCommandRegistry.h"
#include "entity/EntityDef.h"
#include "entity/EntityId.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "loop/GameLoop.h"
#include "render/RenderSnapshot.h"
#include "render/SimRenderBridge.h"
#include "weather/WeatherController.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// Parsing helpers
// ---------------------------------------------------------------------------

static bool parseDouble(std::string_view sv, double& out) {
    // strtod requires a null-terminated string; copy into a fixed buffer.
    // std::from_chars for double is not supported on Apple Clang.
    if (sv.empty() || sv.size() >= 64)
        return false;
    char buf[64];
    std::memcpy(buf, sv.data(), sv.size());
    buf[sv.size()] = '\0';
    char* end = nullptr;
    double v = std::strtod(buf, &end);
    if (end == buf + sv.size() && end != buf) {
        out = v;
        return true;
    }
    return false;
}

static bool parseUint(std::string_view sv, uint32_t& out) {
    uint32_t v{};
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
    if (ec == std::errc{} && ptr == sv.data() + sv.size()) {
        out = v;
        return true;
    }
    return false;
}

static bool isAllDigits(std::string_view sv) {
    if (sv.empty())
        return false;
    for (char c : sv)
        if (c < '0' || c > '9')
            return false;
    return true;
}

// ---------------------------------------------------------------------------
// registerBuiltinCommands
// ---------------------------------------------------------------------------

void registerBuiltinCommands(DebugCommandRegistry& registry, DebugCommandContext ctx) {
    // ------------------------------------------------------------------
    // help [command]
    // ------------------------------------------------------------------
    registry.registerCommand("help", "list all commands, or 'help <cmd>' for details",
                             [&registry](std::span<std::string_view> args) -> std::string {
                                 if (!args.empty()) {
                                     std::string h = registry.helpFor(args[0]);
                                     if (h.empty())
                                         return "unknown command: " + std::string(args[0]);
                                     return std::string(args[0]) + ": " + h;
                                 }
                                 return registry.helpText();
                             });

    // ------------------------------------------------------------------
    // types
    // ------------------------------------------------------------------
    registry.registerCommand(
        "types", "list all registered entity types", [ctx](std::span<std::string_view>) -> std::string {
            if (!ctx.typeRegistry)
                return "types: no type registry";
            uint32_t n = ctx.typeRegistry->typeCount();
            if (n == 0)
                return "(no types registered)";
            std::ostringstream out;
            for (uint32_t i = 0; i < n; ++i) {
                const fl::EntityDef* def = ctx.typeRegistry->byIndex(i);
                if (!def)
                    continue;
                char line[256];
                std::snprintf(line, sizeof(line), "  [%u] %s -- %s", i, def->id.c_str(), def->name.c_str());
                out << line << '\n';
            }
            return out.str();
        });

    // ------------------------------------------------------------------
    // entities
    // ------------------------------------------------------------------
    registry.registerCommand("entities", "list all live entities (idx, type, position)",
                             [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx.renderBridge)
                                     return "entities: no render bridge";
                                 if (!ctx.renderBridge->hasSnapshot())
                                     return "entities: no snapshot yet";
                                 const auto& entries = ctx.renderBridge->current().entries;
                                 if (entries.empty())
                                     return "(no live entities)";
                                 std::ostringstream out;
                                 for (const auto& e : entries) {
                                     const char* typeName = "?";
                                     if (ctx.typeRegistry) {
                                         const fl::EntityDef* def = ctx.typeRegistry->byIndex(e.typeIndex);
                                         if (def)
                                             typeName = def->id.c_str();
                                     }
                                     char line[256];
                                     std::snprintf(line, sizeof(line), "  [%u/%u] %s  X:%+.1f Y:%+.1f Z:%+.1f",
                                                   e.entityIdx, e.entityGen, typeName, static_cast<float>(e.position.x),
                                                   static_cast<float>(e.position.y), static_cast<float>(e.position.z));
                                     out << line << '\n';
                                 }
                                 return out.str();
                             });

    // ------------------------------------------------------------------
    // spawn <type> <x> <y> <z>
    // ------------------------------------------------------------------
    registry.registerCommand("spawn", "spawn <type> <x> <y> <z>  -- spawn entity at world pos",
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.size() < 4)
                                     return "usage: spawn <type> <x> <y> <z>";
                                 if (!ctx.entityManager || !ctx.typeRegistry || !ctx.gameLoop)
                                     return "spawn: not available in this context";

                                 std::string typeArg(args[0]);
                                 double x{}, y{}, z{};
                                 if (!parseDouble(args[1], x) || !parseDouble(args[2], y) || !parseDouble(args[3], z))
                                     return "spawn: invalid coordinates";

                                 // Validate type exists on the main thread before queuing
                                 bool found = false;
                                 if (isAllDigits(typeArg)) {
                                     uint32_t idx{};
                                     if (parseUint(typeArg, idx) && ctx.typeRegistry->byIndex(idx))
                                         found = true;
                                 }
                                 if (!found && ctx.typeRegistry->findById(typeArg.c_str()))
                                     found = true;
                                 if (!found)
                                     return "spawn: unknown type '" + typeArg + "'";

                                 ctx.gameLoop->enqueueSimCallback([em = ctx.entityManager, typeArg, x, y, z] {
                                     fl::EntityTransform t{};
                                     t.pos[0] = x;
                                     t.pos[1] = y;
                                     t.pos[2] = z;
                                     em->spawn(typeArg.c_str(), t);
                                 });
                                 return "spawn queued: " + typeArg;
                             });

    // ------------------------------------------------------------------
    // kill <idx>
    // ------------------------------------------------------------------
    registry.registerCommand("kill", "kill <idx>  -- remove entity from simulation",
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.size() < 1)
                                     return "usage: kill <idx>";
                                 if (!ctx.entityManager || !ctx.renderBridge || !ctx.gameLoop)
                                     return "kill: not available in this context";

                                 uint32_t idx{};
                                 if (!parseUint(args[0], idx))
                                     return "kill: invalid entity index";

                                 // Look up generation from the render bridge snapshot
                                 if (!ctx.renderBridge->hasSnapshot())
                                     return "kill: no snapshot";
                                 uint32_t gen = 0;
                                 for (const auto& e : ctx.renderBridge->current().entries) {
                                     if (e.entityIdx == idx) {
                                         gen = e.entityGen;
                                         break;
                                     }
                                 }
                                 if (gen == 0)
                                     return "kill: entity not found: " + std::string(args[0]);

                                 fl::EntityId id{idx, gen};
                                 ctx.gameLoop->enqueueSimCallback([em = ctx.entityManager, id] { em->kill(id); });

                                 char buf[64];
                                 std::snprintf(buf, sizeof(buf), "kill queued: #%u", idx);
                                 return buf;
                             });

    // ------------------------------------------------------------------
    // tp <x> <y> <z>
    // ------------------------------------------------------------------
    registry.registerCommand("tp", "tp <x> <y> <z>  -- teleport player entity",
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.size() < 3)
                                     return "usage: tp <x> <y> <z>";
                                 if (!ctx.entityManager || !ctx.gameLoop)
                                     return "tp: not available in this context";
                                 if (!ctx.playerEntityIdx || !ctx.playerEntityGen)
                                     return "tp: player entity unknown";
                                 if (*ctx.playerEntityIdx == 0 && *ctx.playerEntityGen == 0)
                                     return "tp: no player entity";

                                 double x{}, y{}, z{};
                                 if (!parseDouble(args[0], x) || !parseDouble(args[1], y) || !parseDouble(args[2], z))
                                     return "tp: invalid coordinates";

                                 fl::EntityId id{*ctx.playerEntityIdx, *ctx.playerEntityGen};
                                 ctx.gameLoop->enqueueSimCallback([em = ctx.entityManager, id, x, y, z] {
                                     fl::EntityState* s = em->get(id);
                                     if (s) {
                                         s->transform.pos[0] = x;
                                         s->transform.pos[1] = y;
                                         s->transform.pos[2] = z;
                                     }
                                 });

                                 char buf[128];
                                 std::snprintf(buf, sizeof(buf), "tp queued: X:%+.1f Y:%+.1f Z:%+.1f",
                                               static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                                 return buf;
                             });

    // ------------------------------------------------------------------
    // toggle_pos
    // ------------------------------------------------------------------
    registry.registerCommand("toggle_pos", "toggle world-position readout (top-right)",
                             [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx.showPos)
                                     return "toggle_pos: not available";
                                 *ctx.showPos = !*ctx.showPos;
                                 return *ctx.showPos ? "pos display: ON" : "pos display: OFF";
                             });

    // ------------------------------------------------------------------
    // set_weather <preset>
    // ------------------------------------------------------------------
    registry.registerCommand(
        "set_weather", "set_weather <clear|partly_cloudy|overcast|rain|storm>  -- set weather preset",
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.empty())
                return "usage: set_weather <clear|partly_cloudy|overcast|rain|storm>";
            if (!ctx.weatherController || !ctx.gameLoop)
                return "set_weather: not available in this context";
            fl::WeatherPreset p;
            if (args[0] == "clear")
                p = fl::WeatherPreset::Clear;
            else if (args[0] == "partly_cloudy")
                p = fl::WeatherPreset::PartlyCloudy;
            else if (args[0] == "overcast")
                p = fl::WeatherPreset::Overcast;
            else if (args[0] == "rain")
                p = fl::WeatherPreset::Rain;
            else if (args[0] == "storm")
                p = fl::WeatherPreset::Storm;
            else
                return "set_weather: unknown preset '" + std::string(args[0]) + "'";
            ctx.gameLoop->enqueueSimCallback([wc = ctx.weatherController, p] { wc->setPreset(p); });
            return "weather preset queued: " + std::string(args[0]);
        });

    // ------------------------------------------------------------------
    // set_difficulty <level>  (stub)
    // ------------------------------------------------------------------
    registry.registerCommand("set_difficulty", "set_difficulty <recruit|cadet|veteran|ace>  -- (stub, Phase 2b)",
                             [](std::span<std::string_view>) -> std::string {
                                 return "set_difficulty: difficulty integration planned for Phase 2b";
                             });

    // ------------------------------------------------------------------
    // reload_content  (stub)
    // ------------------------------------------------------------------
    registry.registerCommand("reload_content", "evict asset cache and reload from content packs  -- (stub, see #152)",
                             [](std::span<std::string_view>) -> std::string {
                                 return "reload_content: asset hot-reload not yet implemented (see issue #152)";
                             });
}
