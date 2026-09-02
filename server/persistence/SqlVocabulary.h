// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The words both backends spell the same way (#534), in one place.
//
// D24 keeps per-backend migration SCRIPTS because the DDL genuinely differs (STRICT vs typed, BLOB
// vs BYTEA, REAL vs DOUBLE PRECISION). Query vocabulary is not DDL, and the parts below are
// identical in both dialects — so they live here rather than in each store, because the alternative
// is two copies of a 25-name column list that must stay in the same ORDER as each other AND as the
// PilotLogbook field walk. That is three things to keep in step, and a mismatch would not fail to
// compile; it would write a pilot's naval kills into their ground-attack column.
//
// Only the placeholder syntax differs (`?` vs `$n`), so the SQL is built from the shared list at
// startup rather than written out twice.
//
// Internal to server/persistence/ — not part of the store's public surface.

#include "Repositories.h"

#include <array>
#include <string>

namespace fl::persist {

// The enums are stored as WORDS, not ordinals: a store an operator can read with the sqlite3 shell
// or psql is worth more than three bytes a row, and an ordinal silently renumbered by a later edit
// to the enum would reinterpret every existing row.
[[nodiscard]] inline const std::string& effectText(RuleEffect effect) {
    static const std::string kDeny = "deny";
    static const std::string kAllow = "allow";
    return effect == RuleEffect::Allow ? kAllow : kDeny;
}

[[nodiscard]] inline const std::string& kindText(SubjectKind kind) {
    static const std::string kIp = "ip";
    static const std::string kAccount = "account";
    return kind == SubjectKind::Account ? kAccount : kIp;
}

// Unknown text reads as the SAFE value in both cases: an unrecognised effect denies, an
// unrecognised subject kind is an IP. A row this build does not understand must not silently become
// an allow rule.
[[nodiscard]] inline RuleEffect effectFromText(const std::string& text) {
    return text == "allow" ? RuleEffect::Allow : RuleEffect::Deny;
}

[[nodiscard]] inline SubjectKind kindFromText(const std::string& text) {
    return text == "account" ? SubjectKind::Account : SubjectKind::Ip;
}

// The account_stats VALUE columns, in the one order every reader and writer walks. This order is
// load-bearing: it must match the PilotLogbook field walk in both stores' Stats repositories, and
// the round-trip test in test_persistence is what proves it does.
inline constexpr std::array<const char*, 25> kStatValueColumns{
    "kills_class_0",      "kills_class_1",       "kills_class_2",    "kills_class_3",      "kills_class_4",
    "kills_class_5",      "kills_class_6",       "kills_class_7",    "air_gun_shots",      "air_gun_hits",
    "air_gun_kills",      "air_missile_shots",   "air_missile_hits", "air_missile_kills",  "ground_attack_shots",
    "ground_attack_hits", "ground_attack_kills", "naval_shots",      "naval_hits",         "naval_kills",
    "missions_flown",     "missions_failed",     "ejections",        "best_landing_score", "last_landing_score",
};

// "kills_class_0, kills_class_1, ..." — the SELECT list, in walk order.
[[nodiscard]] inline std::string statsSelectColumns() {
    std::string out;
    for (std::size_t i = 0; i < kStatValueColumns.size(); ++i) {
        if (i)
            out += ", ";
        out += kStatValueColumns[i];
    }
    return out;
}

// The upsert, with `?` placeholders (SQLite) or `$n` (PostgreSQL). Columns are account_id, then the
// 25 values in walk order, then updated_at.
[[nodiscard]] inline std::string statsUpsertSql(bool numberedPlaceholders) {
    const auto placeholder = [numberedPlaceholders](std::size_t oneBased) {
        return numberedPlaceholders ? "$" + std::to_string(oneBased) : std::string("?");
    };

    std::string cols = "account_id";
    std::string vals = placeholder(1);
    std::string sets;
    for (std::size_t i = 0; i < kStatValueColumns.size(); ++i) {
        cols += ", ";
        cols += kStatValueColumns[i];
        vals += ", " + placeholder(i + 2);
        if (!sets.empty())
            sets += ", ";
        sets += std::string(kStatValueColumns[i]) + " = excluded." + kStatValueColumns[i];
    }
    cols += ", updated_at";
    vals += ", " + placeholder(kStatValueColumns.size() + 2);
    sets += ", updated_at = excluded.updated_at";

    return "INSERT INTO account_stats (" + cols + ") VALUES (" + vals + ") " +
           "ON CONFLICT (account_id) DO UPDATE SET " + sets + ";";
}

} // namespace fl::persist
