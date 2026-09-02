// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The optional PostgreSQL backend (#533, D24, FL_WITH_POSTGRES).
//
// It exists for the operator running several fl-servers against one store — the shape Stage 5's
// deployment work assumes — and it is configure-optional so that Windows and macOS never need libpq
// to build a complete server. The header is always visible; the implementation compiles only when
// the option is on, and openPostgresStore() then answers a plain refusal explaining which build
// flag is missing rather than pretending the backend is unavailable for a runtime reason.

#include "IPersistence.h"

#include <cstddef>
#include <memory>
#include <string>

namespace fl {
class ILogger;
}

namespace fl::persist {

struct PostgresOptions {
    // A libpq connection string or URI. It carries a password, so it is read from server.toml the
    // way other secrets are and never logged back out — see redactDsn().
    std::string dsn;
    std::size_t writeQueueMax{4096};
};

// True when this build actually has the backend compiled in.
[[nodiscard]] bool postgresBackendAvailable();

// Same contract as openSqliteStore: null on failure, with `error` in operator-facing terms.
[[nodiscard]] std::unique_ptr<IPersistence> openPostgresStore(const PostgresOptions& options, ILogger* log,
                                                              std::string& error);

// A DSN with any password= / URI userinfo removed, for logs and the admin surface. Exposed (and
// tested) because the one place a connection string reliably leaks is the log line that reports it.
[[nodiscard]] std::string redactDsn(std::string_view dsn);

} // namespace fl::persist
