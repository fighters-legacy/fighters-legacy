// SPDX-License-Identifier: GPL-3.0-or-later
#include "debug/DebugCommandRegistry.h"

#include <algorithm>
#include <cstring>
#include <sstream>

void DebugCommandRegistry::registerCommand(std::string name, std::string helpText, CommandHandler handler) {
    m_entries.push_back({std::move(name), std::move(helpText), std::move(handler)});
}

std::string DebugCommandRegistry::dispatch(std::string_view line) const {
    auto tokens = tokenize(line);
    if (tokens.empty())
        return {};

    std::string_view cmd = tokens[0];
    auto args = std::span<std::string_view>(tokens).subspan(1);

    for (const auto& e : m_entries) {
        if (e.name == cmd)
            return e.handler(args);
    }

    return "unknown command: " + std::string(cmd) + "  (type 'help' for list)";
}

std::string DebugCommandRegistry::helpText() const {
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

std::string DebugCommandRegistry::helpFor(std::string_view name) const {
    for (const auto& e : m_entries) {
        if (e.name == name)
            return e.help;
    }
    return {};
}

std::vector<std::string_view> DebugCommandRegistry::tokenize(std::string_view line) {
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
