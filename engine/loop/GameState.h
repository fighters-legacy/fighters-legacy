// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

// Named game states that drive music playlist transitions.
// main.cpp calls MusicManager::setState() directly; no observer/manager class needed.
enum class GameState : uint8_t { Menu, FlightPatrol, FlightCombat, MissionSuccess, Debrief };

// Validate a wire byte (MsgMusicState::state, #413) before casting it to GameState.
[[nodiscard]] inline bool isGameStateOrdinal(uint8_t v) noexcept {
    return v <= static_cast<uint8_t>(GameState::Debrief);
}

} // namespace fl
