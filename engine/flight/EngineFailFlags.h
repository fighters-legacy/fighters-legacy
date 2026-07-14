// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

// Engine-failure bitmask for FlightState::engineFailFlags — the single vocabulary shared by the
// flight model (which now RAISES these flags for the asymmetric-thrust model, #675), the render
// snapshot (EntityRenderEntry::engineFailFlags) and the wire codec. A tiny header so engine-flight
// and engine-render both see it without either linking the other.
constexpr uint8_t kEngineFailGeneric = 0x01; // generic thrust impairment (from damage tier)
constexpr uint8_t kEngineFailLeft = 0x02;    // left-engine failure (asymmetric thrust, #675)
constexpr uint8_t kEngineFailRight = 0x04;   // right-engine failure (asymmetric thrust, #675)
constexpr uint8_t kEngineCompStall = 0x08;   // compressor stall
constexpr uint8_t kEngineFlameout = 0x10;    // flameout (total thrust loss)

} // namespace fl
