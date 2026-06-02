// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

class DebugCommandRegistry;
class GameLoop;

namespace fl {
class EntityManager;
class EntityTypeRegistry;
class SimRenderBridge;
} // namespace fl

// Context passed to registerBuiltinCommands(). All pointers except gameLoop
// may be nullptr; commands that need a missing pointer will return an error string.
struct DebugCommandContext {
    fl::EntityManager* entityManager{nullptr};     // sim-thread: spawn/kill/tp lambdas
    fl::EntityTypeRegistry* typeRegistry{nullptr}; // types / entities commands
    fl::SimRenderBridge* renderBridge{nullptr};    // entities command (main-thread read)
    uint32_t* playerEntityIdx{nullptr};            // tp command: player EntityId::index
    uint32_t* playerEntityGen{nullptr};            // tp command: player EntityId::generation
    bool* showPos{nullptr};                        // toggle_pos command
    GameLoop* gameLoop{nullptr};                   // enqueueSimCallback for mutating cmds
};

// Register all built-in debug commands (help, types, entities, spawn, kill,
// tp, toggle_pos, set_weather, set_difficulty, reload_content) against registry.
void registerBuiltinCommands(DebugCommandRegistry& registry, DebugCommandContext ctx);
