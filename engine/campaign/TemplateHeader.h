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

// The ONE `template:` header/body split (#1238). The header parser and materializeMissionTemplate
// used to walk the block with two divergent line-walkers that disagreed on what ends the header —
// a CRLF-authored blank line (a lone '\r') terminated one and continued the other, so validate and
// materialize saw different documents. The boundary, defined once: a line is blank iff it holds
// only spaces, tabs or '\r'; blank and indented lines belong to the header; the first column-0
// non-blank line after `template:` ends it.
struct TemplateSplit {
    std::string header; // "template:\n" + its indented/blank body; empty when no header exists
    std::string body;   // every other line, in order (lines before the header included)
};
[[nodiscard]] TemplateSplit splitTemplateHeader(std::string_view yaml);

[[nodiscard]] TemplateHeaderResult parseTemplateHeader(std::string_view templateYaml);

} // namespace fl
