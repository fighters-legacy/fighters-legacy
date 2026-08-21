// SPDX-License-Identifier: GPL-3.0-or-later
#include "campaign/TemplateHeader.h"

#include <string>
#include <yaml-cpp/yaml.h>

namespace fl {

TemplateSplit splitTemplateHeader(std::string_view yaml) {
    TemplateSplit out;
    bool inHeader = false;
    bool headerDone = false;
    std::size_t pos = 0;
    while (pos < yaml.size()) {
        const std::size_t nl = yaml.find('\n', pos);
        const std::string_view line = (nl == std::string_view::npos) ? yaml.substr(pos) : yaml.substr(pos, nl - pos);

        if (!inHeader && !headerDone && line.rfind("template:", 0) == 0) {
            inHeader = true;
            out.header += "template:\n"; // normalized: the parser reads the block, never this line's tail
        } else if (inHeader) {
            // The one boundary definition (#1238): blank = only spaces/tabs/'\r' (a CRLF-authored
            // blank line arrives as a lone '\r' and is still blank); blank and indented lines
            // belong to the header; the first column-0 non-blank line ends it.
            const bool blank = line.find_first_not_of(" \t\r") == std::string_view::npos;
            const bool indented = !line.empty() && (line[0] == ' ' || line[0] == '\t');
            if (blank || indented) {
                out.header += std::string(line);
                out.header += '\n';
            } else {
                inHeader = false;
                headerDone = true;
                out.body += std::string(line);
                out.body += '\n';
            }
        } else {
            out.body += std::string(line);
            out.body += '\n';
        }

        if (nl == std::string_view::npos)
            break;
        pos = nl + 1;
    }
    return out;
}

TemplateHeaderResult parseTemplateHeader(std::string_view templateYaml) {
    TemplateHeaderResult r;
    const std::string block = splitTemplateHeader(templateYaml).header;
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
