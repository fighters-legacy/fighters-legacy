// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Command handler: receives tokenized args (not including the command name itself).
// Returns a string displayed in the console output (empty = no output).
using CommandHandler = std::function<std::string(std::span<std::string_view> args)>;

// Registry for debug console commands. Commands are registered once at init;
// the registry is read-only (const dispatch) during the game loop.
class DebugCommandRegistry {
  public:
    void registerCommand(std::string name, std::string helpText, CommandHandler handler);

    // Tokenize line on ASCII whitespace and dispatch to the matching handler.
    // Empty / whitespace-only input returns empty string (no dispatch).
    // Unknown command returns an error string.
    [[nodiscard]] std::string dispatch(std::string_view line) const;

    // Multi-line help string listing all registered commands and their help text.
    [[nodiscard]] std::string helpText() const;

    // Single-command help, or empty if the command is not registered.
    [[nodiscard]] std::string helpFor(std::string_view name) const;

  private:
    struct Entry {
        std::string name;
        std::string help;
        CommandHandler handler;
    };

    std::vector<Entry> m_entries;

    static std::vector<std::string_view> tokenize(std::string_view line);
};
