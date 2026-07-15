// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fl {

struct WeaponDef;

struct WeaponValidationResult {
    bool ok{true};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Runs the PLAUSIBILITY pass on an already-parsed weapon def (the same checks validateWeapon() applies
// after parsing). Exposed so the compiled-in builtin stores (BuiltinWeapon) — which are C++ structs,
// never TOML files — can be held to the exact validator bar the tool enforces on pack weapons.
WeaponValidationResult checkWeaponPlausibility(const WeaponDef& w);

// Validates one weapon TOML file.
//
// Schema validity is delegated to the RUNTIME parser (parseWeaponDef): the tool's job is to answer
// "will the engine load this?", and the only honest way to answer it is to ask the thing that loads
// it. That costs fail-fast reporting — the parser throws on the first error rather than accumulating
// — and buys the guarantee that the validator and the engine can never disagree about what is valid.
// A weapon this tool passes is a weapon the engine loads.
//
// On top of that, this adds what a parser deliberately does not do: PLAUSIBILITY warnings. A weapon
// can be schema-perfect and still be nonsense (a 400 g missile with a 2 km blast radius), and the
// parser must not reject it — content packs are allowed to be strange on purpose. A warning says so
// without failing the build.
WeaponValidationResult validateWeapon(std::string_view tomlContent);

// Validates every `weapons/*.toml` in a content pack — full schema + plausibility per file, plus
// the one check that only makes sense across files: duplicate weapon ids.
//
// The hardpoint↔weapon cross-check that used to live here moved to `validate-entity --pack`
// (#829): the references live in entity files, so the tool that parses entity files owns them.
// This tool now answers exactly one question — are the pack's WEAPONS valid?
WeaponValidationResult validatePackWeapons(const std::string& packDir);

} // namespace fl
