// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/IEntityController.h"

namespace fl::ai {

// Holds short of the runway: neutral surfaces, idle throttle. The FlightIntegrator's static parking
// hold (#700 / the pre-existing near-idle ground hold) keeps the aircraft stationary, so a hold-short
// aircraft simply sits there until the ATC departure composition (#702) transitions it to takeoff on
// a ClearanceGranted condition. Header-only — it has no state and no math.
class HoldShortController : public fl::IEntityController {
  public:
    fl::ControlInput sample(const fl::EntityState& /*state*/, uint64_t /*tick*/, double /*dt*/,
                            const fl::AiTickContext& /*ctx*/ = {}) override {
        return fl::ControlInput{}; // idle throttle, neutral surfaces
    }
};

} // namespace fl::ai
