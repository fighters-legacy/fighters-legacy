// SPDX-License-Identifier: GPL-3.0-or-later
#include "campaign/TemplateHeader.h"

#include <yaml-cpp/yaml.h>

namespace fl {

namespace {
// Extract just the top-level `template:` block text (the `template:` line + its indented body), so we
// parse only the header, not the whole mission YAML. Mirrors stripTemplateHeader's column-0 discipline.
std::string extractHeaderBlock(std::string_view yaml) {
    std::string block;
    bool in = false;
    size_t pos = 0;
    while (pos <= yaml.size()) {
        size_t nl = yaml.find('\n', pos);
        std::string_view line = yaml.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        if (!in) {
            if (line.rfind("template:", 0) == 0) {
                in = true;
                block += "template:\n";
            }
        } else {
            // Header body lines are indented (start with a space/tab); a column-0 non-blank ends it.
            if (!line.empty() && line[0] != ' ' && line[0] != '\t') {
                break;
            }
            block += std::string(line);
            block += '\n';
        }
        if (nl == std::string_view::npos)
            break;
        pos = nl + 1;
    }
    return block;
}
} // namespace

TemplateHeaderResult parseTemplateHeader(std::string_view templateYaml) {
    TemplateHeaderResult r;
    const std::string block = extractHeaderBlock(templateYaml);
    if (block.empty())
        return r; // no header -> present stays false

    YAML::Node root;
    try {
        root = YAML::Load(block);
    } catch (const YAML::Exception& e) {
        r.errors.push_back(std::string("template header: YAML parse error: ") + e.what());
        return r;
    }
    YAML::Node hdr = root["template"];
    if (!hdr || !hdr.IsMap()) {
        r.errors.push_back("template header: [template] is not a map");
        return r;
    }
    r.present = true;
    if (hdr["role"])
        r.role = hdr["role"].as<std::string>("");

    if (auto fills = hdr["fills"]) {
        if (!fills.IsSequence()) {
            r.errors.push_back("template header: fills must be a sequence");
        } else {
            for (const auto& entry : fills) {
                if (!entry.IsMap() || entry.size() != 1) {
                    r.warnings.push_back("template header: each fills entry must be a single-key map");
                    continue;
                }
                for (const auto& kv : entry) {
                    TemplateFillDecl d;
                    d.name = kv.first.as<std::string>("");
                    if (kv.second.IsMap() && kv.second["from"])
                        d.from = kv.second["from"].as<std::string>("");
                    r.fills.push_back(std::move(d));
                }
            }
        }
    }
    return r;
}

} // namespace fl
