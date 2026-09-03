// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net/Capability.h" // CapabilityMask + CommandIssuer for permission-checked dispatch (#946)

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

// Command handler: receives tokenized args (not including the command name itself).
// Returns a string displayed in the console output (empty = no output).
using CommandHandler = std::function<std::string(std::span<std::string_view> args)>;

// A handler that also sees WHO ran it (#535). Additive on purpose: the ~43 existing registrations
// keep the plain shape, and only the handful of commands whose OUTPUT is a durable record of an
// operator action -- a ban row's created_by -- take this one. Without it the persisted ban that
// replaced banlist.txt would carry nobody's name, which is most of the reason the flat file could
// not stay.
//
// D37 (#948) is what generalizes this: the registry will grow an any/faction capability pair and
// resolve the issuer's scope before the handler. This is the same direction, one command at a time.
using IssuerCommandHandler = std::function<std::string(std::span<std::string_view> args, const CommandIssuer& issuer)>;

// Registry for console commands. Commands are registered once at init;
// the registry is read-only (const dispatch) during the game loop.
class CommandRegistry {
  public:
    // Register a command with a required-capability mask (#946). The permission-checked dispatch
    // overload refuses the command unless the issuer holds every bit in `required`; required == 0
    // is a public command any issuer may run. The plain dispatch(line) ignores `required` entirely
    // (implicit Admin — stdin console / RCON / --admin-token).
    void registerCommand(std::string name, std::string helpText, CapabilityMask required, CommandHandler handler);

    // The same, for a handler that needs the issuer (see IssuerCommandHandler).
    void registerCommand(std::string name, std::string helpText, CapabilityMask required, IssuerCommandHandler handler);

    // Convenience overload: unannotated commands default to Admin-only (kAdminCaps), preserving the
    // pre-#946 all-or-nothing semantics for any command not yet given an explicit capability.
    void registerCommand(std::string name, std::string helpText, CommandHandler handler);

    // Tokenize line on ASCII whitespace and dispatch to the matching handler. Empty / whitespace-only
    // input returns an empty string (no dispatch).
    //
    // Permission-checked, and the ONLY dispatch there is (#1079). There used to be an issuer-less
    // overload that ignored each command's required mask entirely, and three of the six admin frontends
    // used it -- stdin, RCON and the mission `do:` sink -- so capability enforcement was a property of
    // WHICH FRONTEND YOU ARRIVED ON rather than of the command. Deleting it is what makes that
    // impossible; a caller with system authority passes an explicit issuer saying so, which is a
    // statement rather than an omission. Refuses with a clear "permission denied: <cmd> requires <cap>"
    // when the issuer falls short.
    [[nodiscard]] std::string dispatch(std::string_view line, const CommandIssuer& issuer) const;

    // ── the refusal contract ────────────────────────────────────────────────────────────────
    //
    // dispatch() answers a refusal as PROSE, not an error code, so every frontend that has to turn
    // a refusal into a status has to recognise it from the string. Three did, by hand: the REST
    // frontend twice and the MCP endpoint with its own pair of file-static predicates. The prefixes
    // are minted below and matched here, so a reworded refusal cannot start returning 200.
    static constexpr std::string_view kPermissionDeniedPrefix = "permission denied";
    static constexpr std::string_view kUnknownCommandPrefix = "unknown command";

    [[nodiscard]] static constexpr bool isPermissionDenied(std::string_view result) noexcept {
        return result.starts_with(kPermissionDeniedPrefix);
    }
    [[nodiscard]] static constexpr bool isUnknownCommand(std::string_view result) noexcept {
        return result.starts_with(kUnknownCommandPrefix);
    }

    // The required-capability mask for a command (0 if unknown / public). For tests and tooling.
    [[nodiscard]] CapabilityMask requiredCaps(std::string_view name) const;

    // Multi-line help string listing all registered commands and their help text.
    [[nodiscard]] std::string helpText() const;

    // Single-command help, or empty if the command is not registered.
    [[nodiscard]] std::string helpFor(std::string_view name) const;

  private:
    struct Entry {
        std::string name;
        std::string help;
        CapabilityMask required{kAdminCaps};
        // Exactly one of these is set. Two members rather than a variant because the call site is a
        // single `if`, and because a handler that ignores the issuer should not have to name it.
        CommandHandler handler;
        IssuerCommandHandler issuerHandler;
    };

    std::vector<Entry> m_entries;

    static std::vector<std::string_view> tokenize(std::string_view line);
};

} // namespace fl
