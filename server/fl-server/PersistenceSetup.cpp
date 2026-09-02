// SPDX-License-Identifier: GPL-3.0-or-later
#include "PersistenceSetup.h"

#include <IPersistence.h>
#include <NullStore.h>
#include <PostgresStore.h>
#include <SqliteStore.h>

#include <ILogger.h>

#include <cstdio>

namespace fl {

PersistenceOpenResult openConfiguredStore(const ServerConfig& cfg, ILogger* log) {
    PersistenceOpenResult result;

    if (!cfg.persistence.enabled) {
        result.store = persist::makeNullStore();
        if (log)
            log->log(LogLevel::Info, __FILE__, __LINE__,
                     "persistence: disabled ([persistence] enabled = false) -- bans, accounts and stats "
                     "will NOT survive a restart");
        return result;
    }

    std::string detail;
    if (cfg.persistence.backend == "postgres") {
        persist::PostgresOptions opts;
        opts.dsn = cfg.persistence.postgresDsn;
        opts.writeQueueMax = static_cast<std::size_t>(cfg.persistence.writeQueueMax);
        result.store = persist::openPostgresStore(opts, log, detail);
    } else {
        // Anything else is sqlite. The parser has already rejected an unrecognised name and kept the
        // default, so reaching here with a third value is not possible through config -- and if it
        // ever were, defaulting to the backend that always exists is the safe direction.
        persist::SqliteOptions opts;
        opts.path = cfg.persistence.sqlitePath;
        opts.busyTimeoutMs = cfg.persistence.busyTimeoutMs;
        opts.writeQueueMax = static_cast<std::size_t>(cfg.persistence.writeQueueMax);
        result.store = persist::openSqliteStore(opts, log, detail);
    }

    if (!result.store) {
        // One message, composed here rather than at the call site, because it is the last thing an
        // operator sees before the server exits: it has to name the backend, the reason, and the way
        // out. "cannot open the store" on its own sends someone to a search engine.
        char buf[768];
        std::snprintf(buf, sizeof(buf),
                      "persistence: cannot open the %s store: %s. Fix it, or set [persistence] "
                      "enabled = false to run without one -- this server will not start believing "
                      "it persists when it does not.",
                      cfg.persistence.backend.c_str(), detail.c_str());
        result.error = buf;
    }
    return result;
}

} // namespace fl
