// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/FlightModelData.h"

#include <memory>
#include <string_view>

namespace fl {

// Compiled-in flight model for the zero-content-pack sandbox, and the terminal fallback for every
// entity whose flight model cannot be resolved (empty asset name, missing pack, parse failure).
//
// Models a realistic but LOW-PERFORMING subsonic jet trainer (#1334): a real stall, T/W well under
// one, real structural limits, a real fuel budget. The doctrine: an entity defaulted to the builtin
// for any reason flies plausibly at the floor of the performance ladder — it is never promoted to
// UFO status. (The pre-#1334 model was the deliberate opposite: #181's no-stall, T/W ≈ 4
// "builtin:ufo" debug placeholder, retired when the builtins became the shipped zero-pack game.)
//
// The model is authored as flight-model TOML and compiled through FlightModelParser — the same
// schema authority every content pack goes through — so the validator's table rules and fm-trim's
// envelope maths apply to it verbatim. The source string lives in BuiltinFlightModel.cpp;
// tests/test_fm_trim.cpp pins the derived envelope.
struct BuiltinFlightModel {
    static std::shared_ptr<const FlightModelData> get();
};

// Compiled-in helicopter (#1335): a docile light-utility single-main-rotor machine, so the
// HelicopterForceModel is provable with zero content packs and a mission author can spawn a
// rotorcraft that flies plausibly at the floor of the performance ladder (the #1334 doctrine).
// Authored as reduced-schema TOML through FlightModelParser, like the trainer.
struct BuiltinHelicopterModel {
    static std::shared_ptr<const FlightModelData> get();
};

// Compiled-in multirotor (#1335): a large, slow camera-drone-class quad — the MultirotorForceModel's
// zero-pack proof, and the sandbox's stand-in for any small drone. Same doctrine as above.
struct BuiltinMultirotorModel {
    static std::shared_ptr<const FlightModelData> get();
};

// THE resolver for the `builtin:` flight-model namespace (#1335): the single authority both the
// server and the client resolvers intercept BEFORE any pack lookup, so a builtin name can never be
// shadowed by pack content and both sides of the prediction seam agree by construction (the old
// server-only "builtin:carrier-vessel" literal left a piloted builtin:carrier predicting on the
// wrong model). Returns null for any name it does not own — including unknown `builtin:`-prefixed
// names, which the CALLER must treat as an error rather than falling through to the filesystem.
[[nodiscard]] std::shared_ptr<const FlightModelData> builtinFlightModel(std::string_view name);

// Compiled-in carrier vessel model (#38): what "builtin:carrier" sails with, so the whole
// launch/recovery cycle is provable with zero content packs (the armed-sandbox doctrine). A
// Nimitz-class silhouette in round numbers: ~100 kt displacement, 30+ kt flank speed, a slow
// tactical-diameter turn. Resolved by fl-server's flight-model resolver when an EntityDef names
// the asset "builtin:carrier-vessel". Already realistic — untouched by #1334.
struct BuiltinCarrierVesselModel {
    static std::shared_ptr<const FlightModelData> get();
};

} // namespace fl
