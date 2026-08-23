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
constexpr uint8_t kEngineCompStall = 0x08;   // compressor stall (#308: transient, integrator-owned —
                                             // raised on a surge, cleared after recovery)
constexpr uint8_t kEngineFlameout = 0x10;    // flameout, total thrust loss (#308: integrator-owned —
                                             // fuel starvation or above flameout_alt_km, cleared by
                                             // a windmill relight)
constexpr uint8_t kEngineFailCenter = 0x20;  // centreline single-engine kill (#901): TOTAL thrust
                                             // loss and NO yaw — a single-engine airframe has no dead
                                             // side to swing toward. Distinct from Left/Right (which
                                             // are the twin-engine asymmetric case).

// ── decode ───────────────────────────────────────────────────────────────────
//
// The classification below was restated in all four force models (#1258) — fixed-wing,
// multirotor, helicopter and vessel — so the #901 "centreline = total loss, no yaw" grouping had
// to be got right four times, and the next flag bit would have to be classified four times.
//
// Only the DECODE is shared. What each model does afterwards genuinely differs: fixed-wing swings
// a yaw moment toward the dead engine, multirotor rolls toward the dead rotor, the helicopter
// deliberately does neither because its rotor is on the centreline, and the vessel just loses way.
// Those stay where they are.

[[nodiscard]] constexpr bool engineLeftOut(uint8_t fail) noexcept {
    return (fail & kEngineFailLeft) != 0;
}

[[nodiscard]] constexpr bool engineRightOut(uint8_t fail) noexcept {
    return (fail & kEngineFailRight) != 0;
}

// Every way of losing ALL thrust: the generic damage-tier bit, a flameout, a centreline kill
// (#901 — a single-engine airframe has no dead side to swing toward), a compressor surge (#308 —
// total while it lasts, and the integrator clears the bit on recovery), or both sides out at once.
[[nodiscard]] constexpr bool engineTotalLoss(uint8_t fail) noexcept {
    return (fail & (kEngineFailGeneric | kEngineFlameout | kEngineFailCenter | kEngineCompStall)) != 0 ||
           (engineLeftOut(fail) && engineRightOut(fail));
}

// The thrust one failed engine of `count` accounts for. Spelled as a division rather than a
// multiply by a precomputed fraction because that is what the four copies did, and the two are not
// the same float. `count <= 0` means the def did not say, and a twin is the useful assumption.
[[nodiscard]] constexpr float engineLostThrust(float thrust, int count) noexcept {
    return thrust / static_cast<float>(count > 0 ? count : 2);
}

} // namespace fl
