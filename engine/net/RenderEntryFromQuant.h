// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net/SnapshotCodec.h"
#include "render/RenderSnapshot.h"

#include <glm/gtc/quaternion.hpp>

// QuantEntity -> EntityRenderEntry (#41).
//
// This mapping used to live inside ClientNetEventHandler's packet loop, which was fine while the
// network was the only way a decoded entity could reach the renderer. Replay playback is the second
// way, and D7's whole premise is that the renderer cannot tell a replay from a live session -- which
// is only true if both paths produce the same EntityRenderEntry from the same bits. So it is one
// function, and the two callers share it rather than agreeing to stay in sync.
//
// Header-only and stdlib+glm only: engine-protocol stays zero-dependency, and engine-render's
// RenderSnapshot is a POD header, so this adds no link edge for either caller.

namespace fl {

// Fill `re` from a decoded record. Fields the wire does not carry on this record (articulation,
// which arrives on its own TLV and updates on change) are left untouched, so a caller may prime
// `re` from its cache before calling and keep them across the overwrite.
inline void renderEntryFromQuant(const QuantEntity& qe, EntityRenderEntry& re) noexcept {
    re.entityIdx = qe.idx;
    re.entityGen = static_cast<uint16_t>(qe.gen);
    re.typeIndex = qe.typeIndex;
    re.factionIndex = qe.factionIndex;
    re.position = {qe.pos[0], qe.pos[1], qe.pos[2]};
    re.velocity = {qe.vel[0], qe.vel[1], qe.vel[2]};
    // Wire quaternion order is x,y,z,w; the glm::quat constructor is (w,x,y,z).
    re.orientation = glm::quat(qe.quat[3], qe.quat[0], qe.quat[1], qe.quat[2]);
    re.damageLevel = qe.damageLevel;
    re.playerOwned = qe.playerOwned;
    re.throttle = qe.throttle;
    re.fuelPct = qe.fuelPct;
    re.abEngaged = qe.abEngaged;
    re.engineFailFlags = qe.engineFailFlags;
    re.omega = {qe.omega[0], qe.omega[1], qe.omega[2]};
    // The own-record loadout block (#625) travels with omega and only on the receiving peer's own
    // entity. A replay has no receiving peer, so it is simply absent there -- which is correct: the
    // recording is of the world, not of one player's cockpit.
    re.hasLoadout = qe.hasOmega;
    re.selectedStation = qe.selectedStation;
    re.stationRounds = qe.stationRounds;
    re.weaponFlags = qe.weaponFlags;
    re.payloadMassKg = qe.payloadMassKg;
    re.payloadCd0 = qe.payloadCd0;
}

} // namespace fl
