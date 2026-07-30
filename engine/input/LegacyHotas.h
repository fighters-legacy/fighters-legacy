// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AxisConfig.h"
#include "InputBindings.h"
#include "config/ControlsSettings.h" // LegacyHotasAxes — plain data, no platform dependency

namespace fl {

// Fold a pre-#1061 HOTAS axis mapping into the binding table and the axis-config table.
//
// The version 2 -> 3 conversion has to do this, because a version-2 bindings.toml describes NO
// joystick axes at all — they were in `[controls]` in user.toml. Loading such a file and stopping there
// would leave a player who had a working HOTAS with a stick that does nothing, which is a worse
// outcome than the schema change it came from.
//
// `legacy.present == false` means user.toml never named the keys, so the shipped layout is applied
// instead: the four axes still have to reach the table, they just take their default indices.
//
// Bindings are inserted at the FRONT of each action's list, because that is where the shipped defaults
// put them and because it reproduces the pre-#1061 precedence exactly — the old collector applied the
// HOTAS block after the gamepad block, so the stick won.
void migrateLegacyHotas(const LegacyHotasAxes& legacy, InputBindings& bindings, AxisConfigTable& axes);

} // namespace fl
