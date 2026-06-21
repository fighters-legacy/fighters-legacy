// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

class CommandRegistry;

namespace fl {
class EntityTypeRegistry;
class SimRenderBridge;
} // namespace fl

// Context passed to registerConsoleCommands(). All fields may be nullptr/null;
// commands that need a missing field return an error string.
struct CommandContext {
    fl::SimRenderBridge* renderBridge{nullptr};    // entities command
    fl::EntityTypeRegistry* typeRegistry{nullptr}; // types + entities commands
    uint32_t* playerEntityIdx{nullptr};            // tp command: player EntityId::index
    uint32_t* playerEntityGen{nullptr};            // tp command: player EntityId::generation
    bool* showPos{nullptr};                        // toggle_pos command
    bool* showPing{nullptr};                       // show_ping command: toggle the ping (RTT) overlay

    // Server-side commands (spawn, kill, tp, set_weather) are serialised to admin console
    // text and delivered here. nullptr = no local server (multi-player pure-client);
    // those commands return "not available".
    std::function<void(std::string_view)> serverCommand;
};

// Register all built-in debug commands (help, types, entities, spawn, kill,
// tp, toggle_pos, set_weather, set_difficulty, reload_content) against registry.
void registerConsoleCommands(CommandRegistry& registry, CommandContext ctx);
