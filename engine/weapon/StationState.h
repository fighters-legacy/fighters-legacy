// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "weapon/WeaponDef.h"
#include "weapon/WeaponRegistry.h"

#include <cstdint>
#include <vector>

namespace fl {

struct EntityDef;

// The LIVE state of one weapon station (#625) — what EntityDef::Hardpoint (the immutable "what can
// this station carry") deliberately is not. One per hardpoint, born from the default loadout.
struct StationState {
    uint32_t weaponIndex{UINT32_MAX}; // into the WeaponRegistry; UINT32_MAX = the station is empty
    uint16_t rounds{0};               // shots remaining: a gun's magazine, a rail's missile count
};

// A live entity's whole loadout: its stations, its selection, and what the remaining stores cost
// the airframe RIGHT NOW. The payload figures start at the EntityDef's cached default (#812) and
// shrink as stores leave the rails — which is the moment the per-type static payload stops being
// the truth and this becomes it.
struct LoadoutState {
    std::vector<StationState> stations;
    uint8_t selected{255}; // index into `stations`; 255 = nothing selected
    float payloadMassKg{0.f};
    float payloadCd0{0.f};

    [[nodiscard]] bool empty() const noexcept {
        return stations.empty();
    }
    [[nodiscard]] StationState* selectedStation() noexcept {
        return (selected < stations.size()) ? &stations[selected] : nullptr;
    }
};

// Build a live loadout from an entity def's hardpoints and the weapon registry: each station gets
// its DEFAULT store (empty default = empty station), rounds from the weapon's load, and the payload
// sums mass/drag over what is actually mounted — the same skip rules as fl::defaultPayload. Initial
// selection = the first non-empty non-gun station (the thing a pilot would call "selected"), else
// the first non-empty station, else none.
[[nodiscard]] LoadoutState buildLoadout(const EntityDef& def, const WeaponRegistry& weapons);

} // namespace fl
