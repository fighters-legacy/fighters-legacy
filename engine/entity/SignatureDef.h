// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fl {

// How loud an entity is to each kind of sensor — the TARGET side of detection, where SensorDef is
// the observer side.
//
// Values are UNITLESS MULTIPLIERS against a baseline fighter (1.0). A sensor def quotes its range
// against that baseline; a target's signature scales it. Radar detection range scales by
// `sqrt(sig)`, IR and visual linearly (2026-07-12 decision record, docs/architecture.md): the square
// root echoes the fourth-power range dependence of the radar equation closely enough to make stealth
// feel right, without dragging a real radar equation — and its calibration burden — into a content
// pack. So an aircraft with `rcs = 0.01` is seen at a tenth of a radar's baseline range, and one
// with `ir = 2.0` is seen at twice an IRST's.
//
// This POD is why `engine-entity` does NOT depend on `engine-sensor`: the entity layer describes
// what an entity LOOKS LIKE and names the sensors it carries as plain strings. It does not know what
// a sensor is, and the thing being observed should not depend on the thing observing it.
struct SignatureDef {
    float rcs{1.0f};    // seen by type = "radar"
    float ir{1.0f};     // seen by type = "ir"
    float visual{1.0f}; // seen by type = "visual"
    float laser{1.0f};  // seen by type = "laser"
};

// Per-unit acquisition tuning: how good this particular crew is, not how good its hardware is.
//
// `skill` scales probability of detection UP (higher = sees sooner); `reaction` scales the delay
// between detecting and acting UP (higher = SLOWER to respond). They are deliberately separate: a
// veteran spots a contact early and acts on it immediately, a rookie does neither, and a distracted
// SAM crew can see you perfectly well and still be slow off the mark. One "difficulty" scalar
// couldn't express that.
struct AiTuning {
    float skill{0.5f};    // [0, 1]
    float reaction{0.5f}; // [0, 1]
};

} // namespace fl
