// SPDX-License-Identifier: GPL-3.0-or-later
#include "console/CommandRegistry.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace fl {

void CommandRegistry::registerCommand(std::string name, std::string helpText, CapabilityMask required,
                                      CommandHandler handler) {
    m_entries.push_back({std::move(name), std::move(helpText), required, std::move(handler), nullptr});
}

void CommandRegistry::registerCommand(std::string name, std::string helpText, CapabilityMask required,
                                      IssuerCommandHandler handler) {
    m_entries.push_back({std::move(name), std::move(helpText), required, nullptr, std::move(handler)});
}

void CommandRegistry::registerCommand(std::string name, std::string helpText, CommandHandler handler) {
    // Unannotated = Admin-only (pre-#946 semantics preserved for any command without an explicit mask).
    registerCommand(std::move(name), std::move(helpText), kAdminCaps, std::move(handler));
}

std::string CommandRegistry::dispatch(std::string_view line, const CommandIssuer& issuer) const {
    auto tokens = tokenize(line);
    if (tokens.empty())
        return {};

    std::string_view cmd = tokens[0];
    auto args = std::span<std::string_view>(tokens).subspan(1);

    for (const auto& e : m_entries) {
        if (e.name != cmd)
            continue;
        if (!hasCaps(issuer.caps, e.required)) {
            const std::string_view missing = firstMissingCapabilityName(issuer.caps, e.required);
            return std::string(kPermissionDeniedPrefix) + ": " + std::string(cmd) + " requires " + std::string(missing);
        }
        if (e.issuerHandler)
            return e.issuerHandler(args, issuer);
        return e.handler(args);
    }

    return std::string(kUnknownCommandPrefix) + ": " + std::string(cmd) + "  (type 'help' for list)";
}

CapabilityMask CommandRegistry::requiredCaps(std::string_view name) const {
    for (const auto& e : m_entries) {
        if (e.name == name)
            return e.required;
    }
    return 0;
}

std::string CommandRegistry::helpText() const {
    std::ostringstream out;
    std::size_t maxName = 0;
    for (const auto& e : m_entries)
        maxName = std::max(maxName, e.name.size());

    for (const auto& e : m_entries) {
        out << "  " << e.name;
        for (std::size_t i = e.name.size(); i < maxName + 2; ++i)
            out << ' ';
        out << e.help << '\n';
    }
    return out.str();
}

std::string CommandRegistry::helpFor(std::string_view name) const {
    for (const auto& e : m_entries) {
        if (e.name == name)
            return e.help;
    }
    return {};
}

std::vector<std::string_view> CommandRegistry::tokenize(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t i = 0;
    while (i < line.size()) {
        // Skip whitespace
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            ++i;
        if (i >= line.size())
            break;
        // Collect token
        std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t')
            ++i;
        tokens.push_back(line.substr(start, i - start));
    }
    return tokens;
}

} // namespace fl
