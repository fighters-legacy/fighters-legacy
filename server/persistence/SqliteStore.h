// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The SQLite backend (#533, D24) -- the core store, and the one the Phase 5 acceptance bullet is
// measured against. SQLite is vendored unconditionally (see cmake/dependencies.cmake), so this
// backend exists in every configuration of every platform; FL_WITH_POSTGRES adds a second one, it
// never replaces this.

#include "IPersistence.h"

#include <memory>
#include <string>

namespace fl {
class ILogger;
}

namespace fl::persist {

struct SqliteOptions {
    // Path to the database file. Created, with its parent directory, if absent.
    std::string path;
    // How long a statement waits on a locked database before giving up. WAL means readers and the
    // single writer do not block each other, so this is the backstop for checkpointing, not the
    // normal path.
    int busyTimeoutMs{5000};
    // Cap on the writer's queue. See AsyncWriter.h for what happens when it is reached.
    std::size_t writeQueueMax{4096};
};

// Open the store, run migrations to head, and start the writer thread. On failure the returned
// pointer is null and `error` says why in operator-facing terms -- the caller (ServerRuntime) turns
// that into a refusal to start, because a server that believes it is persisting and is not is worse
// than one that will not boot.
[[nodiscard]] std::unique_ptr<IPersistence> openSqliteStore(const SqliteOptions& options, ILogger* log,
                                                            std::string& error);

} // namespace fl::persist
