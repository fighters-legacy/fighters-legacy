// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The typed repositories behind IPersistence (#533, D24), and the small result type they share.
//
// One repository per domain, each speaking in its own row types rather than in rows and columns.
// #533 shipped blobs; #534 adds accounts, access rules and stats alongside the schema and the row
// types that give them meaning. The rule they were held back for still holds: an interface is
// declared here only once something implements it, or a HAL becomes a wishlist.
//
// ⚑ THERE ARE NO NULLABLE COLUMNS IN THIS SCHEMA, and that is deliberate. Every optional value has
// an explicit empty representation instead — "" for an absent realm, 0 for "never expires". SQL
// NULL is a third truth value that has to be handled at every bind and every read, in two dialects,
// and the one thing it would buy here (distinguishing "unset" from "empty") is a distinction none of
// these fields actually make.

#include <config/PilotLogbook.h> // the stats row type IS the logbook — see IStatsRepository

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fl::persist {

// The outcome of an operation that can fail without it being a programming error: a disk that
// filled, a database a newer build already migrated, a Postgres that went away. Not an exception —
// nothing in this subsystem throws across its own seam, and the server's existing error idiom is
// to report and log.
struct Result {
    bool ok{true};
    std::string error;

    [[nodiscard]] static Result success() {
        return {};
    }
    [[nodiscard]] static Result failure(std::string message) {
        return {false, std::move(message)};
    }
    explicit operator bool() const noexcept {
        return ok;
    }
};

// Opaque byte payloads keyed by an application-chosen string.
//
// This is the ONE untyped repository, and it is untyped on purpose: D25 absorbs the campaign
// `.flsave` as an opaque blob column rather than re-modelling a save format that already has a
// versioned writer and reader of its own. Keys are namespaced by their owner ("campaign/<name>"),
// which keeps one flat table honest without inventing a second schema layer over it.
//
// Reads are synchronous on the calling thread. Writes RETURN ONCE QUEUED — see the threading
// contract in IPersistence.h — so a caller that needs the bytes on disk calls IPersistence::flush()
// afterwards.
class IBlobRepository {
  public:
    virtual ~IBlobRepository() = default;

    // nullopt = no such key. An I/O failure also answers nullopt and logs at Error; the two are
    // distinguishable through StoreHealth for an operator, and not worth distinguishing at a call
    // site that has to handle "no saved campaign" anyway.
    [[nodiscard]] virtual std::optional<std::vector<std::byte>> get(std::string_view key) = 0;

    [[nodiscard]] virtual bool exists(std::string_view key) = 0;

    // Every key starting with prefix, sorted. Pass "" for all of them.
    [[nodiscard]] virtual std::vector<std::string> keys(std::string_view prefix) = 0;

    // Insert or replace. Takes the value by value because it outlives this call: it is moved onto
    // the writer thread's queue, and a span or string_view here would be a dangling read later.
    virtual void put(std::string_view key, std::vector<std::byte> value) = 0;

    // Removing an absent key is not an error.
    virtual void remove(std::string_view key) = 0;
};

// -----------------------------------------------------------------------------------------------
// Accounts (#534, D25)
// -----------------------------------------------------------------------------------------------

struct AccountRecord {
    // A UUIDv7 (fl::uuidv7). Opaque: nothing may parse meaning out of it beyond its ordering.
    std::string id;
    // The official-infra seam D25 buys now while it is free. Every row written today says "local";
    // identity (#537) is what gives it a second value, and a realm column added later would have to
    // backfill every existing account and every foreign key pointing at one.
    std::string realm;
    std::string displayName;
    std::int64_t createdAt{0};  // unix seconds
    std::int64_t lastSeenAt{0}; // unix seconds
};

class IAccountRepository {
  public:
    virtual ~IAccountRepository() = default;

    // Mint a new account. The id and timestamps are generated HERE and returned immediately -- the
    // row itself is queued like every other write, so a caller that must read it back in the same
    // breath calls IPersistence::flush() first. That is the price of never blocking on the store,
    // and it is payable because the id does not come from the database: nothing has to round-trip
    // to learn it, which is half the reason D25 chose an opaque generated id over an autoincrement.
    [[nodiscard]] virtual AccountRecord create(std::string_view realm, std::string_view displayName) = 0;

    [[nodiscard]] virtual std::optional<AccountRecord> get(std::string_view id) = 0;

    // Display names are NOT unique -- deliberately. Whether two people may share a name is an
    // identity policy question, and it belongs to the token-format work (#537/#539), not to the
    // table. Until then this answers the most recently seen match, and says so rather than
    // pretending the question has one answer.
    [[nodiscard]] virtual std::optional<AccountRecord> findByName(std::string_view realm,
                                                                  std::string_view displayName) = 0;

    virtual void touchLastSeen(std::string_view id, std::int64_t unixSeconds) = 0;

    // Removes the account AND its stats row (the schema cascades). Queued.
    virtual void remove(std::string_view id) = 0;
};

// -----------------------------------------------------------------------------------------------
// Access rules -- bans and allowlist entries (#534, D25 / #535)
// -----------------------------------------------------------------------------------------------

// D25 asks for a ban table "dual-keyed IP+account from day one, so #535 is written once". An
// allowlist row is the same shape as a ban row -- a subject and a decision -- and #535 has to
// migrate BOTH files, so one table with an effect column serves that intent better than two
// near-identical tables would (John, 2026-09-02). The allowlist gets account-keying for free when
// identity lands, and any later rule attribute is added in one place.
enum class RuleEffect : std::uint8_t { Deny = 0, Allow = 1 };
enum class SubjectKind : std::uint8_t { Ip = 0, Account = 1 };

struct AccessRule {
    RuleEffect effect{RuleEffect::Deny};
    SubjectKind subjectKind{SubjectKind::Ip};
    // A normalized IP (fl::normalizeIp) or an account id, per subjectKind.
    std::string subject;
    std::string realm;     // account rules only; "" for IP rules
    std::string reason;    // operator-facing; "" when none was given
    std::string createdBy; // the issuing admin, for the audit trail; "" when unattributed
    std::int64_t createdAt{0};
    // Unix seconds, or 0 for "never expires". Not a NULL, and not an optional: see the header note.
    std::int64_t expiresAt{0};
};

class IBanRepository {
  public:
    virtual ~IBanRepository() = default;

    // Insert or replace the rule for (effect, subjectKind, subject). Queued.
    virtual void add(const AccessRule& rule) = 0;

    // Removing an absent rule is not an error. Queued.
    virtual void remove(RuleEffect effect, SubjectKind kind, std::string_view subject) = 0;

    // Rules of this effect that are in force at `nowUnixSeconds` -- permanent rows plus unexpired
    // ones.
    //
    // The expiry filter lives HERE, in the repository, rather than in whatever consumes it. That is
    // what makes temporary bans real for #535 at no cost to it: the seam it swaps onto asks for "the
    // bans", and gets the ones that still apply. `now` is a parameter rather than a clock read so
    // the behaviour is testable without waiting, the way RconServer takes an injectable IClock.
    [[nodiscard]] virtual std::vector<AccessRule> active(RuleEffect effect, std::int64_t nowUnixSeconds) = 0;

    // Every rule of this effect, expired ones included -- the admin view, where "this ban lapsed
    // yesterday" is information rather than noise.
    [[nodiscard]] virtual std::vector<AccessRule> all(RuleEffect effect) = 0;
};

// -----------------------------------------------------------------------------------------------
// Per-account career statistics (#534, D25; the live producer is #929)
// -----------------------------------------------------------------------------------------------

// The row type IS fl::PilotLogbook. D25 says server aggregates reuse the PilotLogbook taxonomy
// (#674) as their value vocabulary, and the most literal way to honour that is to store the struct
// rather than a parallel one that has to be kept in step with it. The columns are wide and flat
// (one per field) rather than a key/value side table, because #930's leaderboards want to ORDER BY
// a column, which is the entire reason this lives in SQL instead of in a blob.
//
// ⚠ The struct and the column list are two things that can drift. test_persistence writes a logbook
// with EVERY field set to a distinct value and compares it field by field after a round trip, so a
// column forgotten when the taxonomy grows fails a test rather than silently zeroing a career.
class IStatsRepository {
  public:
    virtual ~IStatsRepository() = default;

    // nullopt = this account has no stats row yet, which is different from a row of zeroes: it
    // means the pilot has not finished a match, not that they finished one and did nothing.
    [[nodiscard]] virtual std::optional<PilotLogbook> get(std::string_view accountId) = 0;

    // Insert or replace. Queued. The caller owns the aggregation -- the repository stores what it is
    // given and never adds to what is there, so a lost or duplicated flush cannot corrupt a career
    // into something no match produced.
    virtual void put(std::string_view accountId, const PilotLogbook& logbook) = 0;
};

} // namespace fl::persist
