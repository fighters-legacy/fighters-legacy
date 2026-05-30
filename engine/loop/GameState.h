// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

// Named game states that drive music playlist transitions.
// main.cpp calls MusicManager::setState() directly; no observer/manager class needed.
enum class GameState : uint8_t { Menu, FlightPatrol, FlightCombat, MissionSuccess, Debrief };
