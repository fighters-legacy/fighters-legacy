// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Parse a dynamic-sortie template's `template:` header block (#847), so validate-campaign can check
// the declared role + fills without re-implementing the schema. The BODY (everything after the header)
// is materialized by materializeMissionTemplate; this only reads the header.

#include <string>
#include <string_view>
#include <vector>

namespace fl {

// One declared fill in a template header: its placeholder name and its optional `from` source
// (frontline / ground_units); fills with no `from` (e.g. player_flight: {size: 2}) are legal.
struct TemplateFillDecl {
    std::string name;
    std::string from; // empty = a literal-parameter fill (no source)
};

struct TemplateHeaderResult {
    bool present{false}; // a `template:` header block was found
    std::string role;
    std::vector<TemplateFillDecl> fills;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

[[nodiscard]] TemplateHeaderResult parseTemplateHeader(std::string_view templateYaml);

} // namespace fl
