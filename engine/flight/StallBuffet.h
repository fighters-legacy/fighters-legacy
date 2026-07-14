// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace fl {

// Stochastic buffet felt when an aircraft is past its stall AoA (#816).
//
// WHY THIS IS A FREE FUNCTION AND NOT INSIDE FlightIntegrator. ClientPrediction deliberately excludes
// the server's turbulence, because turbulence is stochastic on the server and cannot be reproduced
// without a shared seed. A buffet term hidden inside the integrator would therefore be applied on the
// server and NOT on the client -- reintroducing exactly the client/server divergence that #811's
// parity test exists to prevent, and doing it precisely when the aircraft is hardest to fly.
//
// So the buffet is DETERMINISTIC in (entityIdx, tickIndex) -- the same LCG idiom WorldBroadcaster
// already uses for per-entity turbulence -- and BOTH sides fold it into WindInfluence::turbulence_body
// when FlightState::stalled is set. Same seed, same tick, same entity, same buffet. Prediction stays
// bit-for-bit, and the pilot still feels the wing let go.
//
// Returns a body-frame acceleration (m/s^2) to add to turbulence_body.
[[nodiscard]] inline std::array<float, 3> stallBuffet(uint32_t entityIdx, uint64_t tickIndex,
                                                      float amplitude = 6.0f) noexcept {
    // Same hash as WorldBroadcaster's turbulence seed: order-independent, so it is identical across
    // worker counts and platforms.
    uint32_t rng = entityIdx * 0x9E3779B1u + static_cast<uint32_t>(tickIndex) * 0x85EBCA77u +
                   static_cast<uint32_t>(tickIndex >> 32) * 0xC2B2AE3Du;
    rng = rng * 1664525u + 1013904223u;

    auto next = [&rng]() -> float {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>((rng >> 16) & 0xFFu) / 128.f - 1.f; // [-1, 1)
    };

    // Mostly vertical and rolling: a stalled wing drops, it does not shove the aircraft forwards.
    const float heave = next();
    const float roll = next();
    return {amplitude * 0.15f * next(), amplitude * heave, amplitude * 0.5f * roll};
}

} // namespace fl
