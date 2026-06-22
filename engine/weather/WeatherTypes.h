// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

// Server-authoritative weather preset. Values are stored on the wire (MsgWeatherState::preset)
// so the ordering is stable — do not renumber.
enum class WeatherPreset : uint8_t {
    Clear = 0,
    PartlyCloudy = 1, // scattered clouds, ~35% coverage
    Overcast = 2,     // thick cloud deck, ~75% coverage
    Rain = 3,         // dark dense clouds, heavy fog, moderate turbulence
    Storm = 4,        // near-total cover, maximum fog, strong turbulence
    Snow = 5,         // operator-set snow: any altitude, moderate fog
    Blizzard = 6,     // operator-set heavy snow: any altitude, strong turbulence
};

} // namespace fl
