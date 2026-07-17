// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace fl {

// Deterministic per-(entityIdx, tickIndex) weather-turbulence perturbation (body frame), scaled by
// the current turbulence amplitude (m/s).
//
// It is a PURE function of its inputs — no shared RNG state mutated across entities — so it is
// parallel-safe on the server AND byte-identical on the client from the same inputs. That is the
// whole point (#426): the server's turbulence was already deterministic from (entityIdx, tickIndex),
// so the ONLY thing the client lacked to reproduce it exactly was the scalar amplitude, which now
// travels in MsgWeatherState. Before this, client-side prediction predicted ZERO turbulence and
// diverged by the full amplitude every gusty tick; the per-snapshot reconciliation corrected it,
// leaving the visible positional jitter this closes. Same one-function-two-callers discipline as
// StallBuffet.h — a second implementation would be a second chance to disagree.
[[nodiscard]] inline std::array<float, 3> weatherTurbulence(uint32_t entityIdx, uint64_t tickIndex,
                                                            float amplitude) noexcept {
    if (amplitude == 0.f)
        return {0.f, 0.f, 0.f};
    uint32_t rng = entityIdx * 0x9E3779B1u + static_cast<uint32_t>(tickIndex) * 0x85EBCA77u +
                   static_cast<uint32_t>(tickIndex >> 32) * 0xC2B2AE3Du;
    rng = rng * 1664525u + 1013904223u;
    const float r = static_cast<float>((rng >> 16) & 0xFFu) / 128.f - 1.f;
    return {amplitude * r, amplitude * 0.3f * r, amplitude * 0.5f * r};
}

} // namespace fl
