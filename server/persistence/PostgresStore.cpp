// SPDX-License-Identifier: GPL-3.0-or-later
#include "PostgresStore.h"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>

#if FL_WITH_POSTGRES
#include "AsyncWriter.h"
#include "Migrations.h"

#include <ILogger.h>

#include <libpq-fe.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <utility>
#endif

namespace fl::persist {

// Redaction is compiled unconditionally: it is pure string handling with no libpq in it, the
// FL_WITH_POSTGRES=OFF build still parses a dsn out of server.toml in order to reject it, and a
// test that only runs on one build configuration is a test that rots on the others.
std::string redactDsn(std::string_view dsn) {
    std::string out;
    out.reserve(dsn.size());

    // URI form: postgresql://user:password@host/db -- drop the whole userinfo, since the username
    // is not a secret but is not worth a second parsing rule either.
    const auto scheme = dsn.find("://");
    if (scheme != std::string_view::npos) {
        const auto authStart = scheme + 3;
        const auto slash = dsn.find('/', authStart);
        const auto authEnd = slash == std::string_view::npos ? dsn.size() : slash;
        const auto at = dsn.rfind('@', authEnd);
        if (at != std::string_view::npos && at > authStart) {
            out.append(dsn.substr(0, authStart));
            out.append("***@");
            out.append(dsn.substr(at + 1));
            return out;
        }
        return std::string(dsn);
    }

    // Keyword form: host=... password=... dbname=... -- replace the value of every password key,
    // honouring libpq's single-quoted values ('a b' and \' escapes).
    std::size_t i = 0;
    while (i < dsn.size()) {
        const std::size_t keyStart = i;
        while (i < dsn.size() && dsn[i] != '=' && !std::isspace(static_cast<unsigned char>(dsn[i])))
            ++i;
        std::string_view key = dsn.substr(keyStart, i - keyStart);
        while (i < dsn.size() && std::isspace(static_cast<unsigned char>(dsn[i])))
            ++i;
        if (i >= dsn.size() || dsn[i] != '=') {
            out.append(dsn.substr(keyStart, i - keyStart));
            if (i < dsn.size()) {
                out.push_back(dsn[i]);
                ++i;
            }
            continue;
        }
        ++i; // '='
        while (i < dsn.size() && std::isspace(static_cast<unsigned char>(dsn[i])))
            ++i;

        const std::size_t valStart = i;
        if (i < dsn.size() && dsn[i] == '\'') {
            ++i;
            while (i < dsn.size() && dsn[i] != '\'') {
                if (dsn[i] == '\\' && i + 1 < dsn.size())
                    ++i;
                ++i;
            }
            if (i < dsn.size())
                ++i; // closing quote
        } else {
            while (i < dsn.size() && !std::isspace(static_cast<unsigned char>(dsn[i])))
                ++i;
        }

        out.append(key);
        out.push_back('=');
        if (key == "password")
            out.append("***");
        else
            out.append(dsn.substr(valStart, i - valStart));
        while (i < dsn.size() && std::isspace(static_cast<unsigned char>(dsn[i]))) {
            out.push_back(dsn[i]);
            ++i;
        }
    }
    return out;
}

#if !FL_WITH_POSTGRES

bool postgresBackendAvailable() {
    return false;
}

std::unique_ptr<IPersistence> openPostgresStore(const PostgresOptions&, ILogger*, std::string& error) {
    // A build-time absence, said as one. "PostgreSQL is unavailable" would send an operator to
    // check their server and their network for something that was never compiled.
    error = "[persistence] backend = \"postgres\" but this fl-server was built without it -- "
            "reconfigure with -DFL_WITH_POSTGRES=ON, or use the sqlite backend";
    return nullptr;
}

#else

namespace {

// RAII for a libpq result. Every query path returns through one of these, so the "forgot to
// PQclear on the error branch" leak that libpq code collects cannot happen here.
class PgResult {
  public:
    explicit PgResult(PGresult* r) : mRes(r) {}
    ~PgResult() {
        PQclear(mRes);
    }
    PgResult(const PgResult&) = delete;
    PgResult& operator=(const PgResult&) = delete;

    [[nodiscard]] PGresult* get() const {
        return mRes;
    }
    [[nodiscard]] ExecStatusType status() const {
        return PQresultStatus(mRes);
    }
    [[nodiscard]] bool ok(ExecStatusType want) const {
        return mRes && PQresultStatus(mRes) == want;
    }

  private:
    PGresult* mRes{nullptr};
};

std::int64_t nowSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

Result execRaw(PGconn* conn, const char* sql) {
    PgResult res(PQexec(conn, sql));
    const auto st = res.status();
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK)
        return Result::failure(PQerrorMessage(conn));
    return Result::success();
}

class PostgresMigrationTarget final : public IMigrationTarget {
  public:
    explicit PostgresMigrationTarget(PGconn* conn) : mConn(conn) {}

    Result exec(const char* sql) override {
        return execRaw(mConn, sql);
    }

    Result currentVersion(int& out) override {
        PgResult res(PQexec(mConn, "SELECT COALESCE(MAX(version), 0) FROM schema_version;"));
        if (!res.ok(PGRES_TUPLES_OK) || PQntuples(res.get()) != 1)
            return Result::failure(PQerrorMessage(mConn));
        out = std::atoi(PQgetvalue(res.get(), 0, 0));
        return Result::success();
    }

    Result recordVersion(int version, const char* name) override {
        const std::string versionText = std::to_string(version);
        const std::string appliedText = std::to_string(nowSeconds());
        const char* values[] = {versionText.c_str(), name, appliedText.c_str()};
        PgResult res(PQexecParams(mConn, "INSERT INTO schema_version (version, name, applied_at) VALUES ($1, $2, $3);",
                                  3, nullptr, values, nullptr, nullptr, 0));
        if (!res.ok(PGRES_COMMAND_OK))
            return Result::failure(PQerrorMessage(mConn));
        return Result::success();
    }

    [[nodiscard]] const char* versionTableDdl() const override {
        return "CREATE TABLE IF NOT EXISTS schema_version ("
               "  version    INTEGER NOT NULL PRIMARY KEY,"
               "  name       TEXT    NOT NULL,"
               "  applied_at BIGINT  NOT NULL"
               ");";
    }

  private:
    PGconn* mConn;
};

class PostgresStore final : public IPersistence, private IBlobRepository {
  public:
    PostgresStore(std::size_t queueMax, ILogger* log) : mWriter(queueMax, log), mLog(log) {}

    ~PostgresStore() override {
        close();
    }

    Result connect(const PostgresOptions& options) {
        // Two connections for the same reason the SQLite backend has two: the writer thread owns
        // one, readers share the other. libpq connections are explicitly NOT thread-safe to share.
        mWriteConn = PQconnectdb(options.dsn.c_str());
        if (PQstatus(mWriteConn) != CONNECTION_OK)
            return Result::failure(std::string("connecting: ") + PQerrorMessage(mWriteConn));
        mReadConn = PQconnectdb(options.dsn.c_str());
        if (PQstatus(mReadConn) != CONNECTION_OK)
            return Result::failure(std::string("connecting (reader): ") + PQerrorMessage(mReadConn));

        // libpq writes NOTICEs straight to stderr by default, which would put database output
        // outside the server's logging entirely -- unprefixed, unlevelled, and invisible to any
        // operator reading a journal rather than a terminal. Route them through ILogger instead.
        // The processor is called on whichever thread ran the query, so it must not touch anything
        // but the logger, which is thread-safe by ILogger's own contract.
        for (PGconn* conn : {mWriteConn, mReadConn})
            PQsetNoticeProcessor(conn, &PostgresStore::noticeProcessor, this);
        return Result::success();
    }

    static void noticeProcessor(void* self, const char* message) {
        auto* store = static_cast<PostgresStore*>(self);
        if (!store || !store->mLog || !message)
            return;
        char buf[512];
        // libpq's message already ends in a newline; the logger adds its own line structure.
        std::snprintf(buf, sizeof(buf), "persistence: postgres: %s", message);
        for (char& c : buf)
            if (c == '\n' || c == '\r')
                c = ' ';
        store->mLog->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    Result migrate() {
        PostgresMigrationTarget target(mWriteConn);
        if (auto r = runMigrations(target, postgresMigrations(), mLog); !r)
            return r;
        return target.currentVersion(mSchemaVersion);
    }

    void startWriter() {
        mWriter.start();
    }

    IBlobRepository& blobs() override {
        return *this;
    }

    Result flush() override {
        return mWriter.flush();
    }

    void close() override {
        mWriter.stop();
        std::lock_guard<std::mutex> lock(mReadMutex);
        if (mReadConn) {
            PQfinish(mReadConn);
            mReadConn = nullptr;
        }
        if (mWriteConn) {
            PQfinish(mWriteConn);
            mWriteConn = nullptr;
        }
    }

    [[nodiscard]] StoreHealth health() const override {
        const auto s = mWriter.stats();
        StoreHealth h;
        h.open = mWriteConn != nullptr;
        h.schemaVersion = mSchemaVersion;
        h.writesEnqueued = s.enqueued;
        h.writesCompleted = s.completed;
        h.writesFailed = s.failed;
        h.queueDepth = s.depth;
        h.queueHighWater = s.highWater;
        h.lastError = s.lastError;
        return h;
    }

    [[nodiscard]] std::string_view backendName() const override {
        return "postgres";
    }

  private:
    std::optional<std::vector<std::byte>> get(std::string_view key) override {
        const std::string k(key);
        std::lock_guard<std::mutex> lock(mReadMutex);
        if (!mReadConn)
            return std::nullopt;
        const char* values[] = {k.c_str()};
        const int formats[] = {0};
        // Result format 1 = binary, so the BYTEA arrives as bytes rather than as a hex-escaped
        // string that would then have to be decoded (and mis-decoded) here.
        PgResult res(PQexecParams(mReadConn, "SELECT value FROM blobs WHERE key = $1;", 1, nullptr, values, nullptr,
                                  formats, 1));
        if (!res.ok(PGRES_TUPLES_OK)) {
            logError("blobs.get", PQerrorMessage(mReadConn));
            return std::nullopt;
        }
        if (PQntuples(res.get()) == 0)
            return std::nullopt;
        const auto* data = reinterpret_cast<const std::byte*>(PQgetvalue(res.get(), 0, 0));
        const int size = PQgetlength(res.get(), 0, 0);
        if (!data || size <= 0)
            return std::vector<std::byte>{};
        return std::vector<std::byte>(data, data + size);
    }

    bool exists(std::string_view key) override {
        const std::string k(key);
        std::lock_guard<std::mutex> lock(mReadMutex);
        if (!mReadConn)
            return false;
        const char* values[] = {k.c_str()};
        PgResult res(
            PQexecParams(mReadConn, "SELECT 1 FROM blobs WHERE key = $1;", 1, nullptr, values, nullptr, nullptr, 0));
        if (!res.ok(PGRES_TUPLES_OK)) {
            logError("blobs.exists", PQerrorMessage(mReadConn));
            return false;
        }
        return PQntuples(res.get()) > 0;
    }

    std::vector<std::string> keys(std::string_view prefix) override {
        std::vector<std::string> out;
        const std::string p(prefix);
        std::lock_guard<std::mutex> lock(mReadMutex);
        if (!mReadConn)
            return out;
        // starts_with() rather than LIKE with a built pattern: no escaping question, so a key
        // containing '%' or '_' cannot widen the query. Postgres 11+.
        const char* values[] = {p.c_str()};
        PgResult res(PQexecParams(mReadConn, "SELECT key FROM blobs WHERE starts_with(key, $1) ORDER BY key;", 1,
                                  nullptr, values, nullptr, nullptr, 0));
        if (!res.ok(PGRES_TUPLES_OK)) {
            logError("blobs.keys", PQerrorMessage(mReadConn));
            return out;
        }
        const int rows = PQntuples(res.get());
        out.reserve(static_cast<std::size_t>(rows));
        for (int i = 0; i < rows; ++i)
            out.emplace_back(PQgetvalue(res.get(), i, 0));
        return out;
    }

    void put(std::string_view key, std::vector<std::byte> value) override {
        mWriter.enqueue([this, k = std::string(key), v = std::move(value)]() -> Result {
            const std::string appliedText = std::to_string(nowSeconds());
            const char* values[] = {k.c_str(), v.empty() ? "" : reinterpret_cast<const char*>(v.data()),
                                    appliedText.c_str()};
            const int lengths[] = {0, static_cast<int>(v.size()), 0};
            const int formats[] = {0, 1, 0}; // the blob is binary; the other two are text
            PgResult res(PQexecParams(mWriteConn,
                                      "INSERT INTO blobs (key, value, updated_at) VALUES ($1, $2, $3) "
                                      "ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value, "
                                      "updated_at = EXCLUDED.updated_at;",
                                      3, nullptr, values, lengths, formats, 0));
            if (!res.ok(PGRES_COMMAND_OK))
                return Result::failure("blobs.put '" + k + "': " + PQerrorMessage(mWriteConn));
            return Result::success();
        });
    }

    void remove(std::string_view key) override {
        mWriter.enqueue([this, k = std::string(key)]() -> Result {
            const char* values[] = {k.c_str()};
            PgResult res(
                PQexecParams(mWriteConn, "DELETE FROM blobs WHERE key = $1;", 1, nullptr, values, nullptr, nullptr, 0));
            if (!res.ok(PGRES_COMMAND_OK))
                return Result::failure("blobs.remove '" + k + "': " + PQerrorMessage(mWriteConn));
            return Result::success();
        });
    }

    void logError(const char* what, const char* detail) const {
        if (!mLog)
            return;
        char buf[512];
        std::snprintf(buf, sizeof(buf), "persistence: %s failed: %s", what, detail ? detail : "");
        mLog->log(LogLevel::Error, __FILE__, __LINE__, buf);
    }

    PGconn* mWriteConn{nullptr};
    PGconn* mReadConn{nullptr};
    mutable std::mutex mReadMutex;
    AsyncWriter mWriter;
    ILogger* mLog{nullptr};
    int mSchemaVersion{0};
};

} // namespace

bool postgresBackendAvailable() {
    return true;
}

std::unique_ptr<IPersistence> openPostgresStore(const PostgresOptions& options, ILogger* log, std::string& error) {
    if (options.dsn.empty()) {
        error = "[persistence] backend = \"postgres\" requires postgres_dsn";
        return nullptr;
    }
    auto store = std::make_unique<PostgresStore>(options.writeQueueMax, log);
    if (auto r = store->connect(options); !r) {
        error = r.error;
        return nullptr;
    }
    if (auto r = store->migrate(); !r) {
        error = r.error;
        return nullptr;
    }
    store->startWriter();
    if (log) {
        const std::string safe = redactDsn(options.dsn);
        char buf[512];
        std::snprintf(buf, sizeof(buf), "persistence: postgres store open at %s (schema v%d)", safe.c_str(),
                      store->health().schemaVersion);
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }
    return store;
}

#endif // FL_WITH_POSTGRES

} // namespace fl::persist
