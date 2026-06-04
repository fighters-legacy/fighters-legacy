// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

class DebugCommandRegistry;

namespace fl {
class EntityTypeRegistry;
class SimRenderBridge;
} // namespace fl

// Context passed to registerBuiltinCommands(). All fields may be nullptr/null;
// commands that need a missing field return an error string.
struct DebugCommandContext {
    fl::SimRenderBridge* renderBridge{nullptr};    // entities command
    fl::EntityTypeRegistry* typeRegistry{nullptr}; // types + entities commands
    uint32_t* playerEntityIdx{nullptr};            // tp command: player EntityId::index
    uint32_t* playerEntityGen{nullptr};            // tp command: player EntityId::generation
    bool* showPos{nullptr};                        // toggle_pos command

    // Server-side commands (spawn, kill, tp, set_weather) are serialised to admin console
    // text and delivered here. nullptr = no local server (multi-player pure-client);
    // those commands return "not available".
    std::function<void(std::string_view)> serverCommand;
};

// Register all built-in debug commands (help, types, entities, spawn, kill,
// tp, toggle_pos, set_weather, set_difficulty, reload_content) against registry.
void registerBuiltinCommands(DebugCommandRegistry& registry, DebugCommandContext ctx);
