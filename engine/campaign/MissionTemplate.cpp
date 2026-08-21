// SPDX-License-Identifier: GPL-3.0-or-later
#include "campaign/MissionTemplate.h"

#include "campaign/TemplateHeader.h"

namespace fl {

namespace {

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
    const std::string body = splitTemplateHeader(templateYaml).body;

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
