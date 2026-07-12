// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fl {

struct WeaponValidationResult {
    bool ok{true};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

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

// Cross-file check across a content pack: every weapon id named by an entity's [[hardpoints]]
// (`allowed` and `default`) must resolve to a real weapon definition in the pack.
//
// This is the check that has never existed anywhere: parseEntityDef validates that `default` is in
// `allowed`, but nothing verifies either names a weapon that exists. A typo'd id currently produces
// an aircraft with a station that silently carries nothing.
//
// `packDir` is scanned for `weapons/*.toml` (the weapon ids) and `entities/*.toml` (the references).
// A pack with no entities or no weapons is not an error — it just has nothing to cross-check.
WeaponValidationResult validatePackLoadouts(const std::string& packDir);

} // namespace fl
