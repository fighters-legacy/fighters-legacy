// SPDX-License-Identifier: GPL-3.0-or-later
#include "AccessRules.h"

#include "IpListFile.h"

#include <IPersistence.h>

#include <ILogger.h>

#include <cstdio>
#include <vector>

namespace fl {
namespace {

bool storeUsable(const persist::IPersistence* store) {
    // The null store (persistence disabled) answers every read empty and swallows every write, so
    // treating it as usable would import a file into nothing and then report the ban list as empty.
    return store != nullptr && store->health().open;
}

// The durable "this file has been imported" marker, per effect.
//
// ⚠ IT CANNOT BE "the store holds no rules of this kind", which is what this originally tested and
// what its own test caught within minutes. Unbanning the LAST ban leaves the store legitimately
// empty — and an emptiness check would then re-import the file and resurrect every ban the operator
// had just lifted, on the next restart, with nothing in the log to explain it. Whether the import
// HAPPENED and whether the store is currently empty are different questions.
const char* importMarkerKey(persist::RuleEffect effect) {
    return effect == persist::RuleEffect::Allow ? "import/allowlist" : "import/banlist";
}

AccessRuleLoad load(const AccessRuleSource& src, persist::RuleEffect effect, std::int64_t nowUnixSeconds,
                    const char* what, ILogger* log) {
    AccessRuleLoad out;

    const bool usable = storeUsable(src.store);
    if (usable) {
        const auto rules = src.store->bans().active(effect, nowUnixSeconds);
        for (const auto& r : rules) {
            // Account-keyed rules exist in the schema from day one (D25) but have no subject to
            // match on until identity lands (#537/#538): the connect gauntlet knows an address, not
            // an account. Skipping them here is correct and temporary — #929/#950 give them a
            // consumer — and the rule stays in the record either way.
            if (r.subjectKind == persist::SubjectKind::Ip)
                out.ips.insert(r.subject);
        }
        out.fromStore = true;

        // Already imported: the store is the record, and the file on disk is a frozen souvenir of
        // the upgrade. Reading it again could only ever undo what has happened since.
        if (src.store->blobs().exists(importMarkerKey(effect)))
            return out;
    }

    if (src.importPath.empty())
        return out;

    auto fromFile = loadIpListFile(src.importPath, log);

    if (usable) {
        // The marker is written whether or not the file had anything in it, so "read once" is
        // literally true: an operator who adds lines to the file afterwards is told by the
        // deprecation warning that it is ignored, and it is.
        const std::string note = "imported " + std::to_string(fromFile.size()) + " entries from " + src.importPath +
                                 " at " + std::to_string(nowUnixSeconds);
        src.store->blobs().put(importMarkerKey(effect),
                               std::vector<std::byte>(reinterpret_cast<const std::byte*>(note.data()),
                                                      reinterpret_cast<const std::byte*>(note.data()) + note.size()));
    }

    if (fromFile.empty())
        return out;

    if (usable) {
        for (const auto& ip : fromFile) {
            persist::AccessRule rule;
            rule.effect = effect;
            rule.subjectKind = persist::SubjectKind::Ip;
            rule.subject = ip;
            rule.reason = "imported from " + src.importPath;
            rule.createdBy = "import";
            rule.createdAt = nowUnixSeconds;
            src.store->bans().add(rule);
        }
        out.importedFile = true;
        if (log) {
            char buf[320];
            std::snprintf(buf, sizeof(buf),
                          "%s: imported %zu entries from %s into the store (the file is left in place and "
                          "is no longer read)",
                          what, fromFile.size(), src.importPath.c_str());
            log->log(LogLevel::Info, __FILE__, __LINE__, buf);
        }
    }
    out.ips.insert(fromFile.begin(), fromFile.end());
    return out;
}

} // namespace

AccessRuleLoad loadBanRules(const AccessRuleSource& src, std::int64_t nowUnixSeconds, ILogger* log) {
    return load(src, persist::RuleEffect::Deny, nowUnixSeconds, "banlist", log);
}

AccessRuleLoad loadAllowRules(const AccessRuleSource& src, std::int64_t nowUnixSeconds, ILogger* log) {
    return load(src, persist::RuleEffect::Allow, nowUnixSeconds, "allowlist", log);
}

void recordBan(persist::IPersistence* store, const std::string& ip, const std::string& issuer,
               const std::string& reason, std::int64_t nowUnixSeconds) {
    if (!storeUsable(store))
        return;
    persist::AccessRule rule;
    rule.effect = persist::RuleEffect::Deny;
    rule.subjectKind = persist::SubjectKind::Ip;
    rule.subject = ip;
    rule.reason = reason;
    rule.createdBy = issuer;
    rule.createdAt = nowUnixSeconds;
    // expiresAt stays 0 (permanent). The schema and the repository support expiry, but nothing sets
    // it yet on purpose (#535 scope, John 2026-09-03): a temporary ban would not actually lapse
    // until a restart, because the broadcaster holds the ban set in memory and only rebuilds it on
    // load. `--for` arrives with the refresh loop that makes it true.
    store->bans().add(rule);
}

void removeBan(persist::IPersistence* store, const std::string& ip) {
    if (!storeUsable(store))
        return;
    store->bans().remove(persist::RuleEffect::Deny, persist::SubjectKind::Ip, ip);
}

} // namespace fl
