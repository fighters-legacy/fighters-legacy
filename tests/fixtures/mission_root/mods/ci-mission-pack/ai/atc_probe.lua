-- SPDX-FileCopyrightText: Contributors to Fighters Legacy
-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- The #1288 parity probe: a MISSION-attached script that calls atc.scramble. Before the
-- construction/wiring split, the mission path was handed a null AtcService and this call was a
-- silent no-op, so the run ended with only the mission's own object alive. It now launches a
-- departure from the builtin airfield, and the harness asserts the extra aircraft exists.
--
-- Retried until it succeeds rather than fired once: atc.* returns false rather than throwing when
-- there is nothing behind it, so a single attempt would make a regression look like a timing flake.
scrambled = false

function compute_control(state, tick, dt)
    if not scrambled then
        scrambled = atc.scramble("builtin:airfield", "builtin:debug-entity", 1)
    end
    return {}
end
