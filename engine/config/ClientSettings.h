// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>

struct ClientSettings {
    uint32_t motdDisplayS{15};    // [0, 3600]; 0 = no auto-dismiss
    std::string operatorPassword; // empty = admin commands not available in multiplayer
};
