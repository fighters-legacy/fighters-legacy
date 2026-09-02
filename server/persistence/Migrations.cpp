// SPDX-License-Identifier: GPL-3.0-or-later
#include "Migrations.h"

#include <ILogger.h>

#include <cstdio>

namespace fl::persist {
namespace {

// -------------------------------------------------------------------------------------------
// SQLite
// -------------------------------------------------------------------------------------------
//
// STRICT is on every table. Without it SQLite applies type affinity — a TEXT column accepts an
// integer and keeps it as an integer — so a bug that writes the wrong type produces a row that
// reads back as the wrong type forever, with no error at either end. STRICT needs 3.37+; the
// amalgamation is pinned well past that in cmake/dependencies.cmake, so this is not a portability
// question here the way it would be against a system SQLite.
constexpr Migration kSqliteMigrations[] = {
    {1, "blobs",
     "CREATE TABLE IF NOT EXISTS blobs ("
     "  key        TEXT    NOT NULL PRIMARY KEY,"
     "  value      BLOB    NOT NULL,"
     "  updated_at INTEGER NOT NULL"
     ") STRICT;"},
};

// -------------------------------------------------------------------------------------------
// PostgreSQL
// -------------------------------------------------------------------------------------------
constexpr Migration kPostgresMigrations[] = {
    {1, "blobs",
     "CREATE TABLE IF NOT EXISTS blobs ("
     "  key        TEXT   NOT NULL PRIMARY KEY,"
     "  value      BYTEA  NOT NULL,"
     "  updated_at BIGINT NOT NULL"
     ");"},
};

void logAt(ILogger* log, LogLevel level, const char* message) {
    if (log)
        log->log(level, __FILE__, __LINE__, message);
}

} // namespace

std::span<const Migration> sqliteMigrations() {
    return kSqliteMigrations;
}

std::span<const Migration> postgresMigrations() {
    return kPostgresMigrations;
}

Result runMigrations(IMigrationTarget& target, std::span<const Migration> migrations, ILogger* log) {
    if (migrations.empty())
        return Result::failure("migration set is empty");

    if (auto r = target.exec(target.versionTableDdl()); !r)
        return Result::failure("creating the schema_version table: " + r.error);

    int current = 0;
    if (auto r = target.currentVersion(current); !r)
        return Result::failure("reading the schema version: " + r.error);

    const int head = migrations.back().version;
    if (current > head) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "the store is at schema version %d but this build only knows %d -- it was "
                      "migrated by a NEWER fl-server. Run that build, or restore a backup taken "
                      "before the upgrade; this one will not touch it.",
                      current, head);
        return Result::failure(buf);
    }

    if (current == head) {
        if (log) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "persistence: schema at version %d (up to date)", current);
            logAt(log, LogLevel::Info, buf);
        }
        return Result::success();
    }

    for (const auto& m : migrations) {
        if (m.version <= current)
            continue;
        // Each step is its own transaction: a set that fails halfway leaves the store at the last
        // COMPLETE version rather than in a state no migration describes. EXCLUSIVE from the first
        // statement -- see beginExclusiveSql() for why a plain BEGIN loses a race that ordinary
        // operation reaches.
        if (auto r = target.exec(target.beginExclusiveSql()); !r)
            return Result::failure("beginning the migration transaction: " + r.error);

        // Re-read INSIDE the transaction. The version read before the loop is a hint; this is the
        // decision. Another process may have applied this very migration between the two reads, and
        // it holds whether we waited on the lock for microseconds or for seconds.
        int applied_version = 0;
        if (auto r = target.currentVersion(applied_version); !r) {
            target.exec("ROLLBACK;");
            return Result::failure("re-reading the schema version: " + r.error);
        }
        if (m.version <= applied_version) {
            // Someone else got there first. That is a success, not a conflict.
            if (auto r = target.exec("COMMIT;"); !r)
                return Result::failure("committing the migration transaction: " + r.error);
            current = applied_version;
            continue;
        }

        Result applied = target.exec(m.sql);
        if (applied)
            applied = target.recordVersion(m.version, m.name);
        if (!applied) {
            target.exec("ROLLBACK;"); // best effort: the real error is the one we return
            char buf[192];
            std::snprintf(buf, sizeof(buf), "migration %d (%s) failed: ", m.version, m.name);
            return Result::failure(buf + applied.error);
        }
        if (auto r = target.exec("COMMIT;"); !r)
            return Result::failure("committing the migration transaction: " + r.error);
        current = m.version;

        char buf[128];
        std::snprintf(buf, sizeof(buf), "persistence: applied migration %d (%s)", m.version, m.name);
        logAt(log, LogLevel::Info, buf);
    }
    return Result::success();
}

} // namespace fl::persist
