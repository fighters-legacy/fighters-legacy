// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Schema migrations for the persistence store (#533, D24).
//
// The RUNNER is shared; the SQL is not. D24 says per-backend migration scripts rather than one
// dialect-lowest-common-denominator set, because the interesting statements are exactly the ones
// the two dialects spell differently (STRICT vs typed columns, BLOB vs BYTEA, INTEGER PRIMARY KEY
// vs BIGSERIAL) and a shared script would have to avoid all of them — which means avoiding the
// integrity features that are the reason to use a database instead of a file.
//
// Forward-only. There is no `down` step and there will not be one: a down migration is code that
// runs exactly once, in an emergency, on data nobody has a copy of, having never been exercised.
// The recovery path for a bad migration is a backup, and the operator docs say so.

#include "Repositories.h"

#include <span>
#include <string>

namespace fl {
class ILogger;
}

namespace fl::persist {

// One forward step. `version` is dense and 1-based; `sql` may contain several statements.
struct Migration {
    int version{0};
    const char* name{nullptr};
    const char* sql{nullptr};
};

// What runMigrations needs from a backend. Deliberately four operations: everything else the
// runner needs (which versions are applied, what to run next) is dialect-free and lives in the
// runner, so a third backend implements this and nothing more.
class IMigrationTarget {
  public:
    virtual ~IMigrationTarget() = default;

    // Run one or more statements. Errors come back in Result; nothing throws.
    virtual Result exec(const char* sql) = 0;

    // The highest applied version, or 0 for a store that has never been migrated. Answers an
    // error only if the version cannot be determined — which is different from "there is none",
    // and the caller must not conflate them and re-run migration 1 over live data.
    virtual Result currentVersion(int& out) = 0;

    virtual Result recordVersion(int version, const char* name) = 0;

    // The DDL that creates the bookkeeping table itself, run before currentVersion(). Per-backend
    // for the same reason the rest is.
    [[nodiscard]] virtual const char* versionTableDdl() const = 0;

    // Begin a transaction that holds the WRITE lock from the first statement, not from the first
    // write. This is load-bearing, and a plain BEGIN is not good enough: two servers starting at
    // once both read version 0, both apply migration 1, and the second one's bookkeeping INSERT
    // violates the primary key -- so it reports a failed migration and, under the refuse-to-start
    // policy, does not come up. Two fl-servers starting simultaneously against one fresh database
    // is an ordinary thing (a restart, a test suite, a second server in the same directory), so the
    // check-then-act has to be atomic rather than merely usually-atomic.
    [[nodiscard]] virtual const char* beginExclusiveSql() const = 0;
};

// Apply every migration newer than the store's current version, each inside its own EXCLUSIVE
// transaction, in ascending order, re-reading the applied version inside that transaction so the
// decision and the write cannot be separated by another process. Idempotent, and safe to run
// concurrently from several processes against one database: the losers of the race observe the
// winner's version and skip.
//
// HARD-ERRORS when the store's version is NEWER than the highest migration this binary knows.
// That case is an operator running an old fl-server against a database a newer one has already
// migrated, and the tempting behaviours — carry on, or "migrate down" — both corrupt data written
// by the newer build. Refusing to start is the only honest answer, and the message names both
// versions so the operator knows which binary to run.
[[nodiscard]] Result runMigrations(IMigrationTarget& target, std::span<const Migration> migrations, ILogger* log);

// The SQLite migration set, in version order.
[[nodiscard]] std::span<const Migration> sqliteMigrations();

// The PostgreSQL migration set. Present in every build (it is just data); only ever run when
// FL_WITH_POSTGRES compiled the backend that consumes it.
[[nodiscard]] std::span<const Migration> postgresMigrations();

// The version both sets must reach. A test asserts each set ends here, so a migration added to one
// backend and forgotten on the other fails the build rather than the operator.
inline constexpr int kSchemaHeadVersion = 1;

} // namespace fl::persist
