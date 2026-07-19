// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Dynamic-sortie template materialization (#635). A campaign template is a normal mission YAML with a
// `template:` header block declaring which fields the generator fills, and `${...}` placeholders that
// refer to filled values (docs/modding/formats.md "what a template is"). The campaign engine resolves
// each fill against the live frontline / ground_units (CampaignEngine::buildDynamicSortie, which fills
// NextMission::fills); this turns a template + those fills into a CONCRETE mission YAML that the
// ordinary mission parser (#632) loads. A generated mission is therefore a plain mission file — it can
// be saved, replayed, hand-edited, and validated like any other, which is what keeps the dynamic path
// debuggable.
//
// Pure text transform (no YAML library, no filesystem): the host reads the template bytes and passes
// them here, so engine-campaign stays trivially unit-testable.

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

// Fill values: fill name -> field -> value string (e.g. fills["target_area"]["pos"] = "[1.0, 2.0, 3.0]").
using TemplateFills = std::map<std::string, std::map<std::string, std::string>>;

// Materialize `templateYaml` into a concrete mission YAML: strip the leading `template:` header block
// and substitute every `${name.field}` (and bare `${name}` when a fill has a "" field) with its value
// from `fills`. An unknown placeholder is left verbatim and, if `warnings` is non-null, reported. The
// result is intended to round-trip through parseMission.
[[nodiscard]] std::string materializeMissionTemplate(std::string_view templateYaml, const TemplateFills& fills,
                                                     std::vector<std::string>* warnings = nullptr);

} // namespace fl
