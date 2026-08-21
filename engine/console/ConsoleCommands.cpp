// SPDX-License-Identifier: GPL-3.0-or-later
#include "console/ConsoleCommands.h"
#include "util/Parse.h"

#include "console/CommandRegistry.h"
#include "entity/EntityDef.h"
#include "entity/EntityTypeRegistry.h"
#include "render/ArtChannel.h"
#include "render/RenderSnapshot.h"
#include "render/SimRenderBridge.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

namespace fl {

// ---------------------------------------------------------------------------
// Parsing helpers
// ---------------------------------------------------------------------------

// Call-shape adapters over the strict parsers in util/Parse.h (#1244). The rule — whole string or
// nothing, no infinities — lives there; these keep the bool/out-param form the command table uses.
static bool parseDouble(std::string_view sv, double& out) {
    return readInto(fl::parseDouble(sv), out);
}

static bool parseUint(std::string_view sv, uint32_t& out) {
    return readInto(fl::parseU32(sv), out);
}

// ---------------------------------------------------------------------------
// registerConsoleCommands
// ---------------------------------------------------------------------------

void registerConsoleCommands(CommandRegistry& registry, CommandContext ctx) {
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
                                 if (!ctx.serverCommand)
                                     return "spawn: not available in this context";

                                 std::string typeArg(args[0]);
                                 double x{}, y{}, z{};
                                 if (!parseDouble(args[1], x) || !parseDouble(args[2], y) || !parseDouble(args[3], z))
                                     return "spawn: invalid coordinates";

                                 // Best-effort type validation before forwarding.
                                 if (ctx.typeRegistry) {
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
                                 }

                                 char cmd[256];
                                 std::snprintf(cmd, sizeof(cmd), "spawn %s %g %g %g", typeArg.c_str(), x, y, z);
                                 ctx.serverCommand(cmd);
                                 return "spawn queued: " + typeArg;
                             });

    // ------------------------------------------------------------------
    // kill <idx>
    // ------------------------------------------------------------------
    registry.registerCommand("kill", "kill <idx>  -- remove entity from simulation",
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.size() < 1)
                                     return "usage: kill <idx>";
                                 if (!ctx.serverCommand)
                                     return "kill: not available in this context";

                                 uint32_t idx{};
                                 if (!parseUint(args[0], idx))
                                     return "kill: invalid entity index";

                                 char cmd[64];
                                 std::snprintf(cmd, sizeof(cmd), "kill %u", idx);
                                 ctx.serverCommand(cmd);

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
                                 if (!ctx.serverCommand)
                                     return "tp: not available in this context";
                                 if (!ctx.playerEntityIdx || !ctx.playerEntityGen)
                                     return "tp: player entity unknown";

                                 double x{}, y{}, z{};
                                 if (!parseDouble(args[0], x) || !parseDouble(args[1], y) || !parseDouble(args[2], z))
                                     return "tp: invalid coordinates";

                                 // Forward with entity index so the server knows which entity to move.
                                 char cmd[256];
                                 std::snprintf(cmd, sizeof(cmd), "tp %u %g %g %g", *ctx.playerEntityIdx, x, y, z);
                                 ctx.serverCommand(cmd);

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
    // show_ping
    // ------------------------------------------------------------------
    registry.registerCommand("show_ping", "toggle ping (RTT) overlay",
                             [ctx](std::span<std::string_view>) -> std::string {
                                 if (!ctx.showPing)
                                     return "show_ping: not available";
                                 *ctx.showPing = !*ctx.showPing;
                                 return *ctx.showPing ? "ping overlay on" : "ping overlay off";
                             });

    // ------------------------------------------------------------------
    // set_weather <preset>
    // ------------------------------------------------------------------
    registry.registerCommand(
        "set_weather", "set_weather <clear|partly_cloudy|overcast|rain|storm|snow|blizzard>  -- set weather preset",
        [ctx](std::span<std::string_view> args) -> std::string {
            if (args.empty())
                return "usage: set_weather <clear|partly_cloudy|overcast|rain|storm|snow|blizzard>";
            if (!ctx.serverCommand)
                return "set_weather: not available in this context";
            const std::string_view preset = args[0];
            if (preset != "clear" && preset != "partly_cloudy" && preset != "overcast" && preset != "rain" &&
                preset != "storm" && preset != "snow" && preset != "blizzard")
                return "set_weather: unknown preset '" + std::string(preset) + "'";
            char cmd[64];
            std::snprintf(cmd, sizeof(cmd), "set_weather %.*s", static_cast<int>(preset.size()), preset.data());
            ctx.serverCommand(cmd);
            return "weather preset queued: " + std::string(preset);
        });

    // detonate (#356) — forwarded verbatim; the server owns parsing and the world.
    registry.registerCommand("detonate",
                             "detonate <x> <y> <z> <blast_radius_m> <damage> [--nuclear]  -- AoE warhead "
                             "at a world position (server-side)",
                             [ctx](std::span<std::string_view> args) -> std::string {
                                 if (args.size() < 5)
                                     return "usage: detonate <x> <y> <z> <blast_radius_m> <damage> [--nuclear]";
                                 if (!ctx.serverCommand)
                                     return "detonate: not available in this context";
                                 std::string cmd = "detonate";
                                 for (const auto& a : args) {
                                     cmd += ' ';
                                     cmd += a;
                                 }
                                 ctx.serverCommand(cmd);
                                 return "detonation queued";
                             });

    // ------------------------------------------------------------------
    // set_difficulty <level>  (stub)
    // ------------------------------------------------------------------
    registry.registerCommand("set_difficulty", "set_difficulty <recruit|cadet|veteran|ace>  -- (stub, Phase 2b)",
                             [](std::span<std::string_view>) -> std::string {
                                 return "set_difficulty: difficulty integration planned for Phase 2b";
                             });

    // ------------------------------------------------------------------
    // reload_content (#152) — force a full client + server content reload
    // ------------------------------------------------------------------
    registry.registerCommand(
        "reload_content", "evict asset caches and reload from content packs",
        [reload = ctx.reloadContent, server = ctx.serverCommand](std::span<std::string_view>) -> std::string {
            std::string msg;
            if (reload)
                msg = reload(); // client-side: evict GPU/prediction caches, re-upload
            if (server) {
                server("reload_content"); // forward to the embedded/remote server (flight models, Lua AI)
                msg += msg.empty() ? "reload_content: forwarded to server" : " (+ server)";
            }
            return msg.empty() ? "reload_content: not available" : msg;
        });

    // ------------------------------------------------------------------
    // art (#841) — scrub one articulation channel of one entity
    // ------------------------------------------------------------------
    registry.registerCommand(
        "art", "art <entityIdx> <channel> <value> | art clear — force an articulation channel",
        [setArt = ctx.setArtChannel, clearArt = ctx.clearArtChannels](std::span<std::string_view> args) -> std::string {
            if (!setArt || !clearArt)
                return "art: not available (no renderer)";
            if (args.size() == 1 && args[0] == "clear") {
                clearArt();
                return "art: overrides cleared";
            }
            if (args.size() != 3)
                return "usage: art <entityIdx> <channel> <value>   |   art clear";

            uint32_t idx = 0;
            if (!parseUint(args[0], idx))
                return "art: entityIdx must be a non-negative integer";

            const ArtChannel channel = artChannelFromName(args[1]);
            if (channel == ArtChannel::kCount) {
                std::string names = "art: unknown channel. Valid:";
                for (std::size_t i = 0; i < kArtChannelCount; ++i)
                    names += " " + std::string(artChannelName(static_cast<ArtChannel>(i)));
                return names;
            }

            double v = 0.0;
            if (!parseDouble(args[2], v))
                return "art: value must be a number";
            const std::string vs(args[2]);

            setArt(idx, static_cast<uint8_t>(channel), static_cast<float>(v));
            std::string out = "art: entity ";
            out += std::to_string(idx);
            out += " ";
            out += std::string(artChannelName(channel));
            out += " = ";
            out += vs;
            return out;
        });
}

} // namespace fl
