// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The host seam for the Lua `world.*` module (#413). Lua AI / mission scripts run on the sim thread,
// so the engine-integration calls they make — spawning, faction relations, mission outcome, music —
// are routed through this struct of std::function hooks that the HOST (fl-server) wires to the real
// subsystems (EntityManager, FactionRegistry, MissionRuntime, the music broadcast). engine-script
// therefore stays decoupled: it owns the bindings and the sandbox guards, the host owns where each
// call goes. Every hook is optional — an unset hook makes the corresponding `world.*` call a safe
// no-op (spawn returns -1), so a LuaController with no WorldApi behaves exactly as before.
//
// Threading: the hooks are invoked from the LuaController's sample() on the sim thread. The host
// wires them to sim-thread-safe operations (direct EntityManager/FactionRegistry mutation) or defers
// (a music broadcast queued for the network flush). None of these hooks may block.

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace fl {

struct WorldApi {
    // Spawn an entity of type `typeId` at world position `pos` (metres, Y up) facing compass heading
    // `headingDeg`, for coalition `side` (empty = neutral). Returns the spawned entity's pool index, or
    // -1 on failure (unknown type, entity cap). The host resolves the side against the mission faction
    // registry and may attach a default controller.
    std::function<int(const std::string& typeId, const std::array<double, 3>& pos, float headingDeg,
                      const std::string& side)>
        spawn;

    // Despawn (kill) the entity at pool index `entityIdx`. No-op on an invalid/dead index.
    std::function<void(int entityIdx)> despawn;

    // Set the symmetric relationship between faction ids `a` and `b`. `rel` is one of
    // "friendly" / "neutral" / "hostile"; an unrecognised value is ignored by the host.
    std::function<void(const std::string& a, const std::string& b, const std::string& rel)> setRelationship;

    // Request a music-state transition on connected clients (#166). `state` is one of
    // "menu" / "patrol" / "combat" / "success" / "debrief"; the host maps it to GameState and
    // broadcasts it. A no-op on a headless server with no clients.
    std::function<void(const std::string& state)> setMusicState;

    // End the current mission with success (true) or failure (false). The host drives the objective
    // state machine to the terminal state (as if a mission_success/mission_failure trigger fired).
    std::function<void(bool success)> setMissionOutcome;

    // Haptic feedback (#128). A script never touches the IInput HAL directly; these route to the host,
    // which resolves "the current player's gamepad" (never a gamepad id from the script) and, on a
    // dedicated server, broadcasts the event to clients to play on their local pad. The engine binding
    // clamps the arguments before calling these (freqs [0,1], duration capped), so an untrusted mod
    // cannot lock rumble on. All three are no-ops when unset.
    std::function<void(float lowFreq, float highFreq, uint32_t durationMs)> rumble;
    std::function<void(float leftTrigger, float rightTrigger, uint32_t durationMs)> rumbleTriggers;
    std::function<void()> stopRumble;
};

} // namespace fl
