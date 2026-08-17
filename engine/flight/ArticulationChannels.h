// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ArticulationChannels — the ONE mapping from a FlightState to the normalized ArtChannel values the
// wire codec and the renderer share (#1195).
//
// #843 wired five actuators to the snapshot TLV by writing the list out by hand, and #841's client
// path wrote the same five out by hand again. `[wing_sweep]` landed afterwards and nothing connected
// the two: `ArtChannel::Sweep` was declared, documented for modders and sampled by SceneRenderer,
// but no code anywhere ever wrote it, so a variable-geometry aircraft rendered permanently at
// `min_deg` for every observer INCLUDING the player's own aircraft. Two hand-written copies of one
// list is why that could happen and stay unnoticed, so there is now one list, here, and both sides
// read it.
//
// Adding the next dark channel (`Bay` is the one that bites next — bomb-bay doors are authored
// already) is a line in fillArtChannels and nothing else.
//
// This header is stdlib-only beyond the flight model itself, and includes the deliberately
// dependency-free render/ArtChannel.h — which its own header comment sanctions: "so the wire codec,
// the flight model, the validators and the renderer can all name a channel".

#include "flight/FlightIntegrator.h"
#include "flight/FlightModelData.h"
#include "render/ArtChannel.h"

#include <algorithm>
#include <cstddef>

namespace fl {

// Wing sweep as the `sweep` clip wants it: 0 = WingSweepData::min_deg (wings fully forward),
// 1 = max_deg (fully aft).
//
// The normalization is deliberately the same one AeroForces::sweepCorrection applies to interpolate
// [wing_sweep.spread] -> [wing_sweep.swept], so the wing is DRAWN in the configuration it is FLOWN
// in rather than in a second, independently-derived one.
//
// A fixed-geometry aircraft has no [wing_sweep] and yields 0, which is exactly artChannelNeutral —
// so it is skipped by the sender, costs no bytes, and decodes to the same pose it has today.
[[nodiscard]] inline float sweepChannelValue(const FlightState& s, const FlightModelData& d) noexcept {
    if (!d.wing_sweep)
        return 0.f;
    const WingSweepData& ws = *d.wing_sweep;
    const float span = ws.max_deg - ws.min_deg;
    if (!(span > 0.f))
        return 0.f; // a degenerate authoring case the parser already rejects; do not divide by it
    return std::clamp((s.current_sweep_deg - ws.min_deg) / span, 0.f, 1.f);
}

// Fill the ArtChannel-indexed array from the simulation's own state.
//
// Only channels the FLIGHT MODEL owns are written. GearCompress* are client-derived from the
// suspension and never sent, and the surface/TVC/bay channels have no writer yet, so they stay at
// their neutral — which is what makes an entity that articulates nothing cost nothing.
inline void fillArtChannels(const FlightState& s, const FlightModelData& d, float out[kArtChannelCount]) noexcept {
    for (std::size_t i = 0; i < kArtChannelCount; ++i)
        out[i] = artChannelNeutral(static_cast<ArtChannel>(i));

    out[static_cast<std::size_t>(ArtChannel::Gear)] = s.articulation.gear;
    out[static_cast<std::size_t>(ArtChannel::Flaps)] = s.articulation.flaps;
    out[static_cast<std::size_t>(ArtChannel::Speedbrake)] = s.articulation.speedbrake;
    out[static_cast<std::size_t>(ArtChannel::Hook)] = s.articulation.hook;
    out[static_cast<std::size_t>(ArtChannel::Canopy)] = s.articulation.canopy;
    out[static_cast<std::size_t>(ArtChannel::Sweep)] = sweepChannelValue(s, d);
}

} // namespace fl
