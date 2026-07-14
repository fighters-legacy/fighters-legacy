// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/Trim.h"

#include <string>
#include <string_view>
#include <vector>

namespace fl {

// The CI gate and the JSON report — the parts of fm-trim that are ABOUT a flight model rather than
// part of one. The trim math itself lives in engine/flight/Trim.h, because the in-game aircraft
// manual (#821) renders the same numbers and must not be able to disagree with the CI gate about
// what the aircraft can do.

struct ExpectEntry {
    std::string metric;     // e.g. "stall_speed_1g_mps", "sustained_g", "ps_mps", "max_lift_g"
    float altitude_m{0.f};  // condition
    float mass_kg{0.f};     // condition
    float expected{0.f};    // published value
    float tolerance{0.05f}; // fractional; 0.05 = +/- 5%

    // Pin the condition the number was published at (#826). The flight manual quotes turn and Ps
    // figures AT a Mach, not maximised over speed, so without these the richest data an aircraft has
    // simply cannot be expressed as a gate row.
    float mach{0.f};        // 0 = maximise over speed (the old behaviour)
    float load_factor{0.f}; // required by ps_mps
    bool afterburner{true}; // the manual's turn/Ps numbers are at max thrust

    // Per-row stores. The T.O. publishes max level Mach BOTH clean (1.63) and with wingtip missiles
    // (1.57) at 36 000 ft, and that delta is the ONLY published check on the entire store-drag path
    // (hardpoints -> WeaponLoad.drag_factor -> PayloadEffect). A single --payload CLI flag applied to
    // every row cannot express two conditions in one file.
    float payload_kg{0.f};
    float payload_cd0{0.f};
};

struct ExpectFailure {
    std::string metric;
    float expected{0.f};
    float actual{0.f};
    float tolerance{0.f};
    std::string detail;
};

struct ExpectResult {
    bool ok{true};
    int checked{0};
    std::vector<ExpectFailure> failures;
    std::vector<std::string> errors; // malformed expectation file, unknown metric, ...
};

// A pack authors the numbers its aircraft's flight manual publishes, with tolerances, and this fails
// the build when the model stops reproducing them. THAT IS #54's ACCEPTANCE CRITERION, MECHANISED --
// and it doubles as the engine's guard against someone "improving" AeroForces.cpp and silently
// changing the performance of every aircraft ever authored.
[[nodiscard]] ExpectResult checkExpectations(const FlightModelData& data, std::string_view expectToml,
                                             const PayloadEffect& payload = {});

// max_mach is enforced HERE rather than in the engine (#816): a model that can outrun its own
// declared limit in level flight is a broken model, not a fast aircraft, and the engine must not
// paper over that with an artificial drag wall.
void checkMaxMach(const FlightModelData& data, ExpectResult& out, const PayloadEffect& payload = {});

// Machine-readable output for the CI gate.
[[nodiscard]] std::string toJson(const FlightModelData& data, const std::vector<TrimPoint>& points,
                                 const std::vector<TrimResult>& results);

} // namespace fl
