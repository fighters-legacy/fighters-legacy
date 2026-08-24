// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ValidatorCli.h"

#include <string>
#include <string_view>
#include <vector>

namespace fl {

// The shared three-field shape (#1277); the name stays for every caller.
using EntityValidationResult = ToolValidationResult;

// Validates one entity TOML file.
//
// Schema validity is delegated to the RUNTIME parser (parseEntityDef), for the same reason
// validate-weapon and validate-sensor delegate to theirs: the tool's job is to answer "will the
// engine load this?", and the only honest way to answer it is to ask the thing that loads it. An
// entity this tool passes is an entity the engine registers.
//
// Single-file mode cannot resolve references (asset names and def ids only mean something against a
// pack), so it parses and warns about the fallbacks that bite silently: an air vehicle with no
// flight model flies the builtin UFO, one with no mesh renders the debug wedge.
EntityValidationResult validateEntity(std::string_view tomlContent);

// Full cross-file validation of a content pack: the check that did not exist when fl-base-pack#24
// merged a broken aircraft with every validator green (#829).
//
// For every entities/*.toml, resolved THROUGH THE REAL CONTENT SYSTEM (FolderContentPack +
// ContentIndex, not a reimplementation of their path rules — the f5e bug WAS a path rule):
//   - the def parses (parseEntityDef, verbatim errors);
//   - every non-empty asset reference resolves to a file the pack actually has: mesh, cockpit,
//     manual, classic.damage_mesh, flight_model, ai_script. Unresolvable is an ERROR, not a
//     warning — an unresolvable flight_model does not fail at runtime, it silently degrades to the
//     builtin model and the aircraft flies wrong rather than not at all;
//   - every sensor id resolves through the id→asset index (or is the builtin eyeball);
//   - every hardpoint `allowed`/`default` weapon id resolves to a weapon def in the pack (or the
//     builtin weapon) — moved here from validate-weapon --pack, because the references live in
//     entity files;
//   - entity ids are unique across the pack.
//
// Wrong-spelling detection: an asset name includes its own subdirectory (`f5e/f5e`, because the
// file is aircraft/f5e/f5e.toml and FolderContentPack prepends only the TYPE directory). There is
// exactly one correct spelling and two plausible wrong ones, and at runtime neither wrong one
// fails loudly. When a reference does not resolve but a correction does, the error says so.
EntityValidationResult validateEntityPack(const std::string& packDir);

} // namespace fl
