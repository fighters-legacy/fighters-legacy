// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/IEntityController.h"

namespace fl::ai {

// Holds short of the runway: neutral surfaces, idle throttle, GEAR DOWN. The FlightIntegrator's
// static parking hold (#700 / the pre-existing near-idle ground hold) keeps the aircraft stationary,
// so a hold-short aircraft simply sits there until the ATC departure composition (#702) transitions
// it to takeoff on a ClearanceGranted condition. Header-only — it has no state and no math.
//
// gear_down is the load-bearing line (#1334): ControlInput defaults it FALSE and the actuator slews
// toward the command, so a bare ControlInput{} silently RETRACTED a parked aircraft's gear — and
// with the gear up, ground contact is a 0.55 g belly scrape the departure then has to out-thrust.
// The old UFO (T/W 3.8) did exactly that and hid it; the trainer (T/W 0.32) was pinned at v=0
// forever, which is how every ATC scramble quietly stopped taking off.
class HoldShortController : public fl::IEntityController {
  public:
    fl::ControlInput sample(const fl::EntityState& /*state*/, uint64_t /*tick*/, double /*dt*/,
                            const fl::AiTickContext& /*ctx*/ = {}) override {
        fl::ControlInput ctrl{};
        ctrl.gear_down = true; // parked on its wheels, not on its belly
        return ctrl;
    }
};

} // namespace fl::ai
