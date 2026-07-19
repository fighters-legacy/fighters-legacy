// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/BallisticForceModel.h"
#include "flight/FlightIntegrator.h"
#include "flight/HelicopterForceModel.h"
#include "flight/MultirotorForceModel.h"
#include "flight/VesselForceModel.h"

namespace fl {

// THE force-model selection seam (#349/#350): binds the force model (and any per-class integrator
// tuning) a FlightModelData's role calls for. Both consumers — the server
// (WorldBroadcaster::addControlledEntity) and client prediction (ClientPrediction's lazy init) —
// MUST route through this one function. Before it existed the selection lived only on the server,
// which was survivable while the only non-fixed-wing role (ballistic) was never player-flown; a
// player-predicted rotorcraft with two hand-maintained copies would silently diverge every tick.
inline void applyForceModelFor(FlightIntegrator& fi, const FlightModelData& d) {
    if (d.isBallistic()) {
        fi.setForceModel(BallisticForceModel::instance());
        fi.setSpeedGuard(8000.0); // an MRBM legitimately outruns the NaN backstop built for aircraft
    } else if (d.isMultirotor()) {
        fi.setForceModel(MultirotorForceModel::instance());
    } else if (d.isHelicopter()) {
        fi.setForceModel(HelicopterForceModel::instance());
    } else if (d.isVessel()) {
        fi.setForceModel(VesselForceModel::instance());
    }
    // Anything else keeps the FixedWingForceModel default bound by the integrator itself.
}

} // namespace fl
