// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace fl {

class CommandRegistry;
class EntityTypeRegistry;
class SimRenderBridge;

// Context passed to registerConsoleCommands(). All fields may be nullptr/null;
// commands that need a missing field return an error string.
struct CommandContext {
    SimRenderBridge* renderBridge{nullptr};    // entities command
    EntityTypeRegistry* typeRegistry{nullptr}; // types + entities commands
    uint32_t* playerEntityIdx{nullptr};        // tp command: player EntityId::index
    uint32_t* playerEntityGen{nullptr};        // tp command: player EntityId::generation
    bool* showPos{nullptr};                    // toggle_pos command
    bool* showPing{nullptr};                   // show_ping command: toggle the ping (RTT) overlay

    // Server-side commands (spawn, kill, tp, set_weather) are serialised to admin console
    // text and delivered here. nullptr = no local server (multi-player pure-client);
    // those commands return "not available".
    std::function<void(std::string_view)> serverCommand;

    // reload_content (#152): force a full client-side content reload (evict caches, re-upload). The
    // game wires it to AssetManager::evictAll + SceneRenderer::invalidateAllAssets + prediction /
    // localization / manual invalidation. Kept as a std::function so engine-console stays free of a
    // renderer/asset dependency. nullptr = client reload unavailable (the command still forwards to
    // the server via serverCommand). Returns a human-readable status.
    std::function<std::string()> reloadContent;

    // Articulation debug scrub (#841): `art <entityIdx> <channel> <value>` forces one channel of one
    // entity, `art clear` releases them. The game wires it to SceneRenderer::setArtChannelOverride.
    // This is how the clip -> sampler -> pose arena -> per-node draw path is demonstrable before the
    // simulation or the wire drive it, and it stays useful afterwards for telling "the clip is wrong"
    // apart from "the sim is wrong". nullptr = unavailable (headless / no renderer).
    std::function<void(uint32_t entityIdx, uint8_t channel, float value)> setArtChannel;
    std::function<void()> clearArtChannels;
};

// Register all built-in debug commands (help, types, entities, spawn, kill,
// tp, toggle_pos, set_weather, set_difficulty, reload_content, art) against registry.
void registerConsoleCommands(CommandRegistry& registry, CommandContext ctx);

} // namespace fl
