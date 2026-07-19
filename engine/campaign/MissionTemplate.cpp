// SPDX-License-Identifier: GPL-3.0-or-later
#include "campaign/MissionTemplate.h"

#include <cctype>
#include <sstream>

namespace fl {

namespace {

// Remove a leading top-level `template:` block: the `template:` line (at column 0) plus every following
// indented (or blank) line, up to the next line that begins at column 0 with non-whitespace. Templates
// put the header first, so this is a simple, robust strip that leaves the mission body untouched.
std::string stripTemplateHeader(std::string_view yaml) {
    std::istringstream is{std::string(yaml)};
    std::ostringstream os;
    std::string line;
    bool inHeader = false;
    bool done = false;
    while (std::getline(is, line)) {
        if (!done && !inHeader) {
            // Detect the header start: a line beginning "template:" at column 0.
            std::string_view sv{line};
            if (sv.rfind("template:", 0) == 0) {
                inHeader = true;
                continue; // drop the `template:` line
            }
            os << line << '\n';
            continue;
        }
        if (inHeader) {
            const bool blank = line.find_first_not_of(" \t\r") == std::string::npos;
            const bool indented = !line.empty() && (line[0] == ' ' || line[0] == '\t');
            if (blank || indented)
                continue; // still inside the header block
            inHeader = false;
            done = true; // the header is fully consumed; the rest is the mission body
        }
        os << line << '\n';
    }
    return os.str();
}

// Look up ${name.field} (or ${name} -> name."") in the fills; returns false if absent.
bool resolvePlaceholder(const TemplateFills& fills, std::string_view token, std::string& out) {
    const auto dot = token.find('.');
    std::string name = std::string(dot == std::string_view::npos ? token : token.substr(0, dot));
    std::string field = dot == std::string_view::npos ? std::string() : std::string(token.substr(dot + 1));
    auto nameIt = fills.find(name);
    if (nameIt == fills.end())
        return false;
    auto fieldIt = nameIt->second.find(field);
    if (fieldIt == nameIt->second.end())
        return false;
    out = fieldIt->second;
    return true;
}

} // namespace

std::string materializeMissionTemplate(std::string_view templateYaml, const TemplateFills& fills,
                                       std::vector<std::string>* warnings) {
    const std::string body = stripTemplateHeader(templateYaml);

    std::string out;
    out.reserve(body.size());
    for (std::size_t i = 0; i < body.size();) {
        if (body[i] == '$' && i + 1 < body.size() && body[i + 1] == '{') {
            const std::size_t close = body.find('}', i + 2);
            if (close != std::string::npos) {
                const std::string_view token(body.data() + i + 2, close - (i + 2));
                std::string value;
                if (resolvePlaceholder(fills, token, value)) {
                    out += value;
                } else {
                    out.append(body, i, close - i + 1); // leave the placeholder verbatim
                    if (warnings)
                        warnings->push_back("unresolved template placeholder: ${" + std::string(token) + "}");
                }
                i = close + 1;
                continue;
            }
        }
        out += body[i++];
    }
    return out;
}

} // namespace fl
