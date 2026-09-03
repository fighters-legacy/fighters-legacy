// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Bans and the allowlist, backed by the persistence store (#535, D25).
//
// This is the acceptance clause of Stage 1 in one file: "bans survive restart; a pre-existing
// banlist.txt imports". Before it, the server's ban list was `banlist.txt` — rewritten in full on
// every ban, from the sim thread, with no reason, no issuer and no expiry.
//
// It lives apart from ServerRuntime for the reason CampaignSave does: the interesting part is the
// PRECEDENCE and the import, and inside a ServerRuntime::Impl method nothing could reach it. The
// import in particular runs exactly once in a deployment's life and must be right the first time.
//
// ⚠ THE STORE IS THE RECORD; THE FILES ARE AN IMPORT SOURCE. `security.banlist_path` and
// `security.allowlist_path` are deprecated to import-only (#535): read once, when the store has no
// rules of that kind, and never written again. The file is LEFT ON DISK — it is an operator's ban
// list, deleting it buys nothing, and a downgrade should still find it — which is exactly why the
// store must win on every subsequent load. See loadAccessRules.

#include <cstdint>
#include <string>
#include <unordered_set>

namespace fl {

class ILogger;

namespace persist {
class IPersistence;
}

// Where the rules of one kind come from.
struct AccessRuleSource {
    persist::IPersistence* store{nullptr};
    // The deprecated file, or "" when none is configured. Import source only.
    std::string importPath;
};

struct AccessRuleLoad {
    std::unordered_set<std::string> ips;
    bool fromStore{false};    // the store held rules (even if the set is empty, see `imported`)
    bool importedFile{false}; // a file was found and its entries were written into the store
};

// Load the ACTIVE deny rules (bans) as normalized IPs, importing the file once if the store has
// none. `nowUnixSeconds` is passed rather than read so expiry is testable; a rule whose expiry has
// passed is simply not returned.
[[nodiscard]] AccessRuleLoad loadBanRules(const AccessRuleSource& src, std::int64_t nowUnixSeconds, ILogger* log);

// The same for allow rules (the allowlist).
[[nodiscard]] AccessRuleLoad loadAllowRules(const AccessRuleSource& src, std::int64_t nowUnixSeconds, ILogger* log);

// Record a ban. `issuer` is the admin the audited command path resolved — it lands in the rule's
// created_by, which is the whole reason the flat file could not stay: a ban a year old with nobody's
// name on it cannot be reviewed.
//
// ⚠ May be called from the SIM thread (the `ban <peerId>` path only learns the IP inside a sim
// callback), so fl-server posts it through GameLoop::enqueueMainCallback. The write itself is
// queued by the store, as every write is.
void recordBan(persist::IPersistence* store, const std::string& ip, const std::string& issuer,
               const std::string& reason, std::int64_t nowUnixSeconds);

// Remove a ban. Removing one that was never there is not an error.
void removeBan(persist::IPersistence* store, const std::string& ip);

} // namespace fl
