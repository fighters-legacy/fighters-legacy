// SPDX-License-Identifier: GPL-3.0-or-later
#include "PostgresStore.h"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>

#if FL_WITH_POSTGRES
#include "AsyncWriter.h"
#include "Migrations.h"
#include "SqlVocabulary.h"

#include <crypto/Uuid.h>

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

    [[nodiscard]] const char* beginExclusiveSql() const override {
        // Postgres has no BEGIN IMMEDIATE; the equivalent is to take the table lock explicitly.
        // Without it two servers in READ COMMITTED both see version 0 and both insert, which is
        // the same race, reported as a duplicate key instead of a UNIQUE constraint.
        return "BEGIN; LOCK TABLE schema_version IN EXCLUSIVE MODE;";
    }

  private:
    PGconn* mConn;
};

class PostgresStore final : public IPersistence {
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
        return mBlobs;
    }
    IAccountRepository& accounts() override {
        return mAccounts;
    }
    IBanRepository& bans() override {
        return mBans;
    }
    IStatsRepository& stats() override {
        return mStats;
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
    // Repositories are MEMBERS, not private bases: four interfaces each declaring
    // get(std::string_view) cannot all be inherited by one class (same signature, different return
    // type is not an overload). Mirrors SqliteStore.

    class Blobs final : public IBlobRepository {
      public:
        explicit Blobs(PostgresStore* store) : s(store) {}

        std::optional<std::vector<std::byte>> get(std::string_view key) override {
            const std::string k(key);
            std::lock_guard<std::mutex> lock(s->mReadMutex);
            if (!s->mReadConn)
                return std::nullopt;
            const char* values[] = {k.c_str()};
            const int formats[] = {0};
            // Result format 1 = binary, so the BYTEA arrives as bytes rather than as a hex-escaped
            // string that would then have to be decoded (and mis-decoded) here.
            PgResult res(PQexecParams(s->mReadConn, "SELECT value FROM blobs WHERE key = $1;", 1, nullptr, values,
                                      nullptr, formats, 1));
            if (!res.ok(PGRES_TUPLES_OK)) {
                s->logError("blobs.get", PQerrorMessage(s->mReadConn));
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
            std::lock_guard<std::mutex> lock(s->mReadMutex);
            if (!s->mReadConn)
                return false;
            const char* values[] = {k.c_str()};
            PgResult res(PQexecParams(s->mReadConn, "SELECT 1 FROM blobs WHERE key = $1;", 1, nullptr, values, nullptr,
                                      nullptr, 0));
            if (!res.ok(PGRES_TUPLES_OK)) {
                s->logError("blobs.exists", PQerrorMessage(s->mReadConn));
                return false;
            }
            return PQntuples(res.get()) > 0;
        }

        std::vector<std::string> keys(std::string_view prefix) override {
            std::vector<std::string> out;
            const std::string p(prefix);
            std::lock_guard<std::mutex> lock(s->mReadMutex);
            if (!s->mReadConn)
                return out;
            // starts_with() rather than LIKE with a built pattern: no escaping question, so a key
            // containing '%' or '_' cannot widen the query. Postgres 11+.
            const char* values[] = {p.c_str()};
            PgResult res(PQexecParams(s->mReadConn, "SELECT key FROM blobs WHERE starts_with(key, $1) ORDER BY key;", 1,
                                      nullptr, values, nullptr, nullptr, 0));
            if (!res.ok(PGRES_TUPLES_OK)) {
                s->logError("blobs.keys", PQerrorMessage(s->mReadConn));
                return out;
            }
            const int rows = PQntuples(res.get());
            out.reserve(static_cast<std::size_t>(rows));
            for (int i = 0; i < rows; ++i)
                out.emplace_back(PQgetvalue(res.get(), i, 0));
            return out;
        }

        void put(std::string_view key, std::vector<std::byte> value) override {
            s->mWriter.enqueue([this, k = std::string(key), v = std::move(value)]() -> Result {
                const std::string appliedText = std::to_string(nowSeconds());
                const char* values[] = {k.c_str(), v.empty() ? "" : reinterpret_cast<const char*>(v.data()),
                                        appliedText.c_str()};
                const int lengths[] = {0, static_cast<int>(v.size()), 0};
                const int formats[] = {0, 1, 0}; // the blob is binary; the other two are text
                PgResult res(PQexecParams(s->mWriteConn,
                                          "INSERT INTO blobs (key, value, updated_at) VALUES ($1, $2, $3) "
                                          "ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value, "
                                          "updated_at = EXCLUDED.updated_at;",
                                          3, nullptr, values, lengths, formats, 0));
                if (!res.ok(PGRES_COMMAND_OK))
                    return Result::failure("blobs.put '" + k + "': " + PQerrorMessage(s->mWriteConn));
                return Result::success();
            });
        }

        void remove(std::string_view key) override {
            s->mWriter.enqueue([this, k = std::string(key)]() -> Result {
                const char* values[] = {k.c_str()};
                PgResult res(PQexecParams(s->mWriteConn, "DELETE FROM blobs WHERE key = $1;", 1, nullptr, values,
                                          nullptr, nullptr, 0));
                if (!res.ok(PGRES_COMMAND_OK))
                    return Result::failure("blobs.remove '" + k + "': " + PQerrorMessage(s->mWriteConn));
                return Result::success();
            });
        }

      private:
        PostgresStore* s;
    };

    // ---- Accounts ----------------------------------------------------------------------------

    class Accounts final : public IAccountRepository {
      public:
        explicit Accounts(PostgresStore* store) : s(store) {}

        AccountRecord create(std::string_view realm, std::string_view displayName) override {
            AccountRecord rec;
            rec.id = fl::uuidv7();
            rec.realm = std::string(realm);
            rec.displayName = std::string(displayName);
            rec.createdAt = nowSeconds();
            rec.lastSeenAt = rec.createdAt;

            s->mWriter.enqueue([this, rec]() -> Result {
                const std::string created = std::to_string(rec.createdAt);
                const std::string seen = std::to_string(rec.lastSeenAt);
                const char* v[] = {rec.id.c_str(), rec.realm.c_str(), rec.displayName.c_str(), created.c_str(),
                                   seen.c_str()};
                PgResult res(PQexecParams(s->mWriteConn,
                                          "INSERT INTO accounts (id, realm, display_name, created_at, last_seen_at) "
                                          "VALUES ($1, $2, $3, $4, $5);",
                                          5, nullptr, v, nullptr, nullptr, 0));
                if (!res.ok(PGRES_COMMAND_OK))
                    return Result::failure("accounts.create '" + rec.id + "': " + PQerrorMessage(s->mWriteConn));
                return Result::success();
            });
            return rec;
        }

        std::optional<AccountRecord> get(std::string_view id) override {
            const std::string k(id);
            std::lock_guard<std::mutex> lock(s->mReadMutex);
            if (!s->mReadConn)
                return std::nullopt;
            const char* v[] = {k.c_str()};
            PgResult res(PQexecParams(s->mReadConn,
                                      "SELECT id, realm, display_name, created_at, last_seen_at "
                                      "FROM accounts WHERE id = $1;",
                                      1, nullptr, v, nullptr, nullptr, 0));
            if (!res.ok(PGRES_TUPLES_OK)) {
                s->logError("accounts.get", PQerrorMessage(s->mReadConn));
                return std::nullopt;
            }
            if (PQntuples(res.get()) == 0)
                return std::nullopt;
            return readRow(res, 0);
        }

        std::optional<AccountRecord> findByName(std::string_view realm, std::string_view displayName) override {
            const std::string r(realm);
            const std::string n(displayName);
            std::lock_guard<std::mutex> lock(s->mReadMutex);
            if (!s->mReadConn)
                return std::nullopt;
            const char* v[] = {r.c_str(), n.c_str()};
            PgResult res(PQexecParams(s->mReadConn,
                                      "SELECT id, realm, display_name, created_at, last_seen_at FROM accounts "
                                      "WHERE realm = $1 AND display_name = $2 "
                                      "ORDER BY last_seen_at DESC, id DESC LIMIT 1;",
                                      2, nullptr, v, nullptr, nullptr, 0));
            if (!res.ok(PGRES_TUPLES_OK)) {
                s->logError("accounts.findByName", PQerrorMessage(s->mReadConn));
                return std::nullopt;
            }
            if (PQntuples(res.get()) == 0)
                return std::nullopt;
            return readRow(res, 0);
        }

        void touchLastSeen(std::string_view id, std::int64_t unixSeconds) override {
            s->mWriter.enqueue([this, k = std::string(id), unixSeconds]() -> Result {
                const std::string when = std::to_string(unixSeconds);
                const char* v[] = {when.c_str(), k.c_str()};
                PgResult res(PQexecParams(s->mWriteConn, "UPDATE accounts SET last_seen_at = $1 WHERE id = $2;", 2,
                                          nullptr, v, nullptr, nullptr, 0));
                if (!res.ok(PGRES_COMMAND_OK))
                    return Result::failure("accounts.touchLastSeen '" + k + "': " + PQerrorMessage(s->mWriteConn));
                return Result::success();
            });
        }

        void remove(std::string_view id) override {
            s->mWriter.enqueue([this, k = std::string(id)]() -> Result {
                const char* v[] = {k.c_str()};
                PgResult res(PQexecParams(s->mWriteConn, "DELETE FROM accounts WHERE id = $1;", 1, nullptr, v, nullptr,
                                          nullptr, 0));
                if (!res.ok(PGRES_COMMAND_OK))
                    return Result::failure("accounts.remove '" + k + "': " + PQerrorMessage(s->mWriteConn));
                return Result::success();
            });
        }

      private:
        static AccountRecord readRow(const PgResult& res, int row) {
            AccountRecord rec;
            rec.id = PQgetvalue(res.get(), row, 0);
            rec.realm = PQgetvalue(res.get(), row, 1);
            rec.displayName = PQgetvalue(res.get(), row, 2);
            rec.createdAt = std::strtoll(PQgetvalue(res.get(), row, 3), nullptr, 10);
            rec.lastSeenAt = std::strtoll(PQgetvalue(res.get(), row, 4), nullptr, 10);
            return rec;
        }
        PostgresStore* s;
    };

    // ---- Access rules ------------------------------------------------------------------------

    class Bans final : public IBanRepository {
      public:
        explicit Bans(PostgresStore* store) : s(store) {}

        void add(const AccessRule& rule) override {
            s->mWriter.enqueue([this, rule]() -> Result {
                const std::string effect = effectText(rule.effect);
                const std::string kind = kindText(rule.subjectKind);
                const std::string created = std::to_string(rule.createdAt);
                const std::string expires = std::to_string(rule.expiresAt);
                const char* v[] = {effect.c_str(),     kind.c_str(),        rule.subject.c_str(),
                                   rule.realm.c_str(), rule.reason.c_str(), rule.createdBy.c_str(),
                                   created.c_str(),    expires.c_str()};
                PgResult res(PQexecParams(s->mWriteConn,
                                          "INSERT INTO access_rules (effect, subject_kind, subject, realm, reason, "
                                          "created_by, created_at, expires_at) "
                                          "VALUES ($1, $2, $3, $4, $5, $6, $7, $8) "
                                          "ON CONFLICT (effect, subject_kind, subject) DO UPDATE SET "
                                          "realm = EXCLUDED.realm, reason = EXCLUDED.reason, "
                                          "created_by = EXCLUDED.created_by, created_at = EXCLUDED.created_at, "
                                          "expires_at = EXCLUDED.expires_at;",
                                          8, nullptr, v, nullptr, nullptr, 0));
                if (!res.ok(PGRES_COMMAND_OK))
                    return Result::failure("bans.add '" + rule.subject + "': " + PQerrorMessage(s->mWriteConn));
                return Result::success();
            });
        }

        void remove(RuleEffect effect, SubjectKind kind, std::string_view subject) override {
            s->mWriter.enqueue([this, effect, kind, subj = std::string(subject)]() -> Result {
                const std::string e = effectText(effect);
                const std::string k = kindText(kind);
                const char* v[] = {e.c_str(), k.c_str(), subj.c_str()};
                PgResult res(PQexecParams(s->mWriteConn,
                                          "DELETE FROM access_rules WHERE effect = $1 AND subject_kind = $2 "
                                          "AND subject = $3;",
                                          3, nullptr, v, nullptr, nullptr, 0));
                if (!res.ok(PGRES_COMMAND_OK))
                    return Result::failure("bans.remove '" + subj + "': " + PQerrorMessage(s->mWriteConn));
                return Result::success();
            });
        }

        std::vector<AccessRule> active(RuleEffect effect, std::int64_t nowUnixSeconds) override {
            return query(effect, "AND (expires_at = 0 OR expires_at > $2)", nowUnixSeconds);
        }

        std::vector<AccessRule> all(RuleEffect effect) override {
            return query(effect, "", std::nullopt);
        }

      private:
        std::vector<AccessRule> query(RuleEffect effect, const char* extraWhere,
                                      std::optional<std::int64_t> nowUnixSeconds) {
            std::vector<AccessRule> out;
            const std::string e = effectText(effect);
            const std::string now = nowUnixSeconds ? std::to_string(*nowUnixSeconds) : std::string();
            std::lock_guard<std::mutex> lock(s->mReadMutex);
            if (!s->mReadConn)
                return out;
            std::string sql = "SELECT effect, subject_kind, subject, realm, reason, created_by, created_at, "
                              "expires_at FROM access_rules WHERE effect = $1 ";
            sql += extraWhere;
            sql += " ORDER BY created_at, subject;";
            const char* v[] = {e.c_str(), now.c_str()};
            PgResult res(
                PQexecParams(s->mReadConn, sql.c_str(), nowUnixSeconds ? 2 : 1, nullptr, v, nullptr, nullptr, 0));
            if (!res.ok(PGRES_TUPLES_OK)) {
                s->logError("bans.query", PQerrorMessage(s->mReadConn));
                return out;
            }
            const int rows = PQntuples(res.get());
            out.reserve(static_cast<std::size_t>(rows));
            for (int i = 0; i < rows; ++i) {
                AccessRule rule;
                rule.effect = effectFromText(PQgetvalue(res.get(), i, 0));
                rule.subjectKind = kindFromText(PQgetvalue(res.get(), i, 1));
                rule.subject = PQgetvalue(res.get(), i, 2);
                rule.realm = PQgetvalue(res.get(), i, 3);
                rule.reason = PQgetvalue(res.get(), i, 4);
                rule.createdBy = PQgetvalue(res.get(), i, 5);
                rule.createdAt = std::strtoll(PQgetvalue(res.get(), i, 6), nullptr, 10);
                rule.expiresAt = std::strtoll(PQgetvalue(res.get(), i, 7), nullptr, 10);
                out.push_back(std::move(rule));
            }
            return out;
        }
        PostgresStore* s;
    };

    // ---- Stats -------------------------------------------------------------------------------

    class Stats final : public IStatsRepository {
      public:
        explicit Stats(PostgresStore* store) : s(store) {}

        std::optional<PilotLogbook> get(std::string_view accountId) override {
            const std::string id(accountId);
            std::lock_guard<std::mutex> lock(s->mReadMutex);
            if (!s->mReadConn)
                return std::nullopt;
            const std::string sql = "SELECT " + statsSelectColumns() + " FROM account_stats WHERE account_id = $1;";
            const char* v[] = {id.c_str()};
            PgResult res(PQexecParams(s->mReadConn, sql.c_str(), 1, nullptr, v, nullptr, nullptr, 0));
            if (!res.ok(PGRES_TUPLES_OK)) {
                s->logError("stats.get", PQerrorMessage(s->mReadConn));
                return std::nullopt;
            }
            if (PQntuples(res.get()) == 0)
                return std::nullopt;

            PilotLogbook lb;
            int c = 0;
            const auto u32 = [&](int col) {
                return static_cast<std::uint32_t>(std::strtoull(PQgetvalue(res.get(), 0, col), nullptr, 10));
            };
            for (int i = 0; i < PilotLogbook::kKillClassCount; ++i)
                lb.killsByClass[i] = u32(c++);
            for (int w = 0; w < static_cast<int>(WeaponLogClass::Count); ++w) {
                lb.weapons[w].shots = u32(c++);
                lb.weapons[w].hits = u32(c++);
                lb.weapons[w].kills = u32(c++);
            }
            lb.missionsFlown = u32(c++);
            lb.missionsFailed = u32(c++);
            lb.ejections = u32(c++);
            lb.bestLandingScore = std::strtof(PQgetvalue(res.get(), 0, c++), nullptr);
            lb.lastLandingScore = std::strtof(PQgetvalue(res.get(), 0, c++), nullptr);
            return lb;
        }

        void put(std::string_view accountId, const PilotLogbook& logbook) override {
            s->mWriter.enqueue([this, id = std::string(accountId), lb = logbook]() -> Result {
                // 27 parameters, built as text and kept alive in `owned` until the exec returns --
                // libpq reads the pointers, it does not copy them.
                std::vector<std::string> owned;
                owned.reserve(27);
                owned.push_back(id);
                for (int i = 0; i < PilotLogbook::kKillClassCount; ++i)
                    owned.push_back(std::to_string(lb.killsByClass[i]));
                for (int w = 0; w < static_cast<int>(WeaponLogClass::Count); ++w) {
                    owned.push_back(std::to_string(lb.weapons[w].shots));
                    owned.push_back(std::to_string(lb.weapons[w].hits));
                    owned.push_back(std::to_string(lb.weapons[w].kills));
                }
                owned.push_back(std::to_string(lb.missionsFlown));
                owned.push_back(std::to_string(lb.missionsFailed));
                owned.push_back(std::to_string(lb.ejections));
                owned.push_back(std::to_string(lb.bestLandingScore));
                owned.push_back(std::to_string(lb.lastLandingScore));
                owned.push_back(std::to_string(nowSeconds()));

                std::vector<const char*> v;
                v.reserve(owned.size());
                for (const auto& o : owned)
                    v.push_back(o.c_str());

                const std::string sql = statsUpsertSql(/*numberedPlaceholders=*/true);
                PgResult res(PQexecParams(s->mWriteConn, sql.c_str(), static_cast<int>(v.size()), nullptr, v.data(),
                                          nullptr, nullptr, 0));
                if (!res.ok(PGRES_COMMAND_OK))
                    return Result::failure("stats.put '" + id + "': " + PQerrorMessage(s->mWriteConn));
                return Result::success();
            });
        }

      private:
        PostgresStore* s;
    };

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

    Blobs mBlobs{this};
    Accounts mAccounts{this};
    Bans mBans{this};
    Stats mStats{this};
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
