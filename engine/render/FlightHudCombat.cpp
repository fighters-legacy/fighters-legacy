// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/FlightHud.h"

// FlightHud::drawCombat — combat symbology (#641): the target designator box, gun pipper with lead,
// CCIP release cue, and off-screen locator arrows. Split out of FlightHud.cpp so the redesign (#438)
// and the combat overlay evolve independently. #438 lands this as an empty stub; #641 fills it in.

namespace fl {

void FlightHud::drawCombat(Ctx& /*c*/) {
    // Intentionally empty until #641. The designated-target box / pipper / CCIP are drawn here from
    // Ctx::in.designatedTarget + the projection helper, gated by Ctx::in.masterArm.
}

} // namespace fl
