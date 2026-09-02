// SPDX-License-Identifier: GPL-3.0-or-later
#include "SqliteStore.h"

#include "AsyncWriter.h"
#include "Migrations.h"

#include <ILogger.h>

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <utility>

namespace fl::persist {
namespace {

// RAII for a prepared statement. Statements are prepared per call rather than cached: this store
// serves bans, accounts and campaign saves -- events measured in per-match, not per-tick -- so a
// statement cache would buy nothing and would have to be made per-connection and thread-aware to be
// correct. What matters here is that every value is BOUND, never formatted into SQL.
class Stmt {
  public:
    Stmt() = default;
    ~Stmt() {
        sqlite3_finalize(mStmt);
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Result prepare(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &mStmt, nullptr) != SQLITE_OK)
            return Result::failure(std::string("prepare: ") + sqlite3_errmsg(db));
        return Result::success();
    }
    [[nodiscard]] sqlite3_stmt* get() const {
        return mStmt;
    }

  private:
    sqlite3_stmt* mStmt{nullptr};
};

Result execRaw(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        Result r = Result::failure(err ? err : "unknown sqlite error");
        sqlite3_free(err);
        return r;
    }
    return Result::success();
}

std::int64_t nowSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// The bookkeeping half of the migration contract, in SQLite's dialect.
class SqliteMigrationTarget final : public IMigrationTarget {
  public:
    explicit SqliteMigrationTarget(sqlite3* db) : mDb(db) {}

    Result exec(const char* sql) override {
        return execRaw(mDb, sql);
    }

    Result currentVersion(int& out) override {
        Stmt s;
        if (auto r = s.prepare(mDb, "SELECT COALESCE(MAX(version), 0) FROM schema_version;"); !r)
            return r;
        const int rc = sqlite3_step(s.get());
        if (rc != SQLITE_ROW)
            return Result::failure(std::string("reading schema_version: ") + sqlite3_errmsg(mDb));
        out = sqlite3_column_int(s.get(), 0);
        return Result::success();
    }

    Result recordVersion(int version, const char* name) override {
        Stmt s;
        if (auto r = s.prepare(mDb, "INSERT INTO schema_version (version, name, applied_at) VALUES (?, ?, ?);"); !r)
            return r;
        sqlite3_bind_int(s.get(), 1, version);
        sqlite3_bind_text(s.get(), 2, name, -1, SQLITE_STATIC);
        sqlite3_bind_int64(s.get(), 3, nowSeconds());
        if (sqlite3_step(s.get()) != SQLITE_DONE)
            return Result::failure(std::string("recording schema version: ") + sqlite3_errmsg(mDb));
        return Result::success();
    }

    [[nodiscard]] const char* versionTableDdl() const override {
        return "CREATE TABLE IF NOT EXISTS schema_version ("
               "  version    INTEGER NOT NULL PRIMARY KEY,"
               "  name       TEXT    NOT NULL,"
               "  applied_at INTEGER NOT NULL"
               ") STRICT;";
    }

  private:
    sqlite3* mDb;
};

class SqliteStore final : public IPersistence, private IBlobRepository {
  public:
    SqliteStore(std::size_t queueMax, ILogger* log) : mWriter(queueMax, log), mLog(log) {}

    ~SqliteStore() override {
        close();
    }

    // Open the WRITE connection and configure it. The read connection deliberately comes later, in
    // openReadConnection() after migrations have run -- see the comment there.
    Result openWriteConnection(const SqliteOptions& options) {
        mPath = options.path;
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        // The path is added by the caller (see withPath in openSqliteStore), so it appears exactly
        // once however deep in the open sequence the failure was.
        if (sqlite3_open_v2(mPath.c_str(), &mWriteDb, flags, nullptr) != SQLITE_OK)
            return Result::failure(mWriteDb ? sqlite3_errmsg(mWriteDb) : "out of memory");

        // Set BEFORE anything that can contend, so the very first statement already waits rather
        // than failing instantly against another process holding the file.
        sqlite3_busy_timeout(mWriteDb, options.busyTimeoutMs);
        if (auto r = execRaw(mWriteDb, "PRAGMA foreign_keys=ON;"); !r)
            return r;

        // WAL IS AN OPTIMIZATION, NOT A CORRECTNESS REQUIREMENT, AND A FAILURE HERE MUST NOT STOP
        // THE SERVER STARTING.
        //
        // It gives readers a snapshot a concurrent write does not block -- an operator listing bans
        // should not wait on a campaign save. But the journal mode is a property of the DATABASE
        // FILE, and the delete->wal transition needs a brief exclusive lock. SQLite returns
        // SQLITE_BUSY for that when another connection has the file open, and it does NOT invoke
        // the busy handler for a journal-mode change -- so the busy_timeout above does not cover
        // it. Several fl-servers sharing a working directory (which is exactly what the test suite
        // does, and what a developer running two servers does) can therefore lose this race.
        //
        // Refusing to start over it would be absurd: the store is fully functional in the rollback
        // journal mode SQLite falls back to, only less concurrent. So this warns and carries on,
        // and says which mode it ended up in rather than leaving the operator to guess.
        if (auto r = execRaw(mWriteDb, "PRAGMA journal_mode=WAL;"); !r && mLog) {
            char buf[384];
            std::snprintf(buf, sizeof(buf),
                          "persistence: could not switch %s to WAL (%s); continuing in its current "
                          "journal mode -- reads may block behind a write",
                          mPath.c_str(), r.error.c_str());
            mLog->log(LogLevel::Warn, __FILE__, __LINE__, buf);
        }
        // NORMAL under WAL: durable across a process crash (which is what `kill -9` in the
        // acceptance test is), at risk only from an OS/power loss, and an order of magnitude
        // cheaper than FULL. The acceptance bullet is process-crash survival, so this is the
        // setting that matches what is actually promised.
        if (auto r = execRaw(mWriteDb, "PRAGMA synchronous=NORMAL;"); !r)
            return r;
        return Result::success();
    }

    // The READ connection, opened after migrations.
    //
    // Two connections exist so the writer thread owns one for the process lifetime while readers
    // share the other under a mutex. This one is READONLY, which cannot create the file -- so it
    // has to be opened once the write connection and the migrations have certainly produced one.
    // Opening it first left a window whose failure mode was "the server will not start" on a
    // perfectly good machine, which is far too severe a consequence for an ordering detail.
    Result openReadConnection(const SqliteOptions& options) {
        if (sqlite3_open_v2(mPath.c_str(), &mReadDb, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
            return Result::failure(std::string("opening for reading: ") +
                                   (mReadDb ? sqlite3_errmsg(mReadDb) : "out of memory"));
        sqlite3_busy_timeout(mReadDb, options.busyTimeoutMs);
        return execRaw(mReadDb, "PRAGMA foreign_keys=ON;");
    }

    Result migrate() {
        SqliteMigrationTarget target(mWriteDb);
        if (auto r = runMigrations(target, sqliteMigrations(), mLog); !r)
            return r;
        return target.currentVersion(mSchemaVersion);
    }

    void startWriter() {
        mWriter.start();
    }

    // ---- IPersistence -------------------------------------------------------------------

    IBlobRepository& blobs() override {
        return *this;
    }

    Result flush() override {
        return mWriter.flush();
    }

    void close() override {
        mWriter.stop();
        std::lock_guard<std::mutex> lock(mReadMutex);
        if (mReadDb) {
            sqlite3_close(mReadDb);
            mReadDb = nullptr;
        }
        if (mWriteDb) {
            sqlite3_close(mWriteDb);
            mWriteDb = nullptr;
        }
    }

    [[nodiscard]] StoreHealth health() const override {
        const auto s = mWriter.stats();
        StoreHealth h;
        h.open = mWriteDb != nullptr;
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
        return "sqlite";
    }

  private:
    // ---- IBlobRepository ----------------------------------------------------------------

    std::optional<std::vector<std::byte>> get(std::string_view key) override {
        std::lock_guard<std::mutex> lock(mReadMutex);
        if (!mReadDb)
            return std::nullopt;
        Stmt s;
        if (auto r = s.prepare(mReadDb, "SELECT value FROM blobs WHERE key = ?;"); !r) {
            logError("blobs.get", r.error);
            return std::nullopt;
        }
        sqlite3_bind_text(s.get(), 1, key.data(), static_cast<int>(key.size()), SQLITE_STATIC);
        const int rc = sqlite3_step(s.get());
        if (rc == SQLITE_DONE)
            return std::nullopt; // no such key -- not an error
        if (rc != SQLITE_ROW) {
            logError("blobs.get", sqlite3_errmsg(mReadDb));
            return std::nullopt;
        }
        const auto* data = static_cast<const std::byte*>(sqlite3_column_blob(s.get(), 0));
        const int size = sqlite3_column_bytes(s.get(), 0);
        if (!data || size <= 0)
            return std::vector<std::byte>{};
        return std::vector<std::byte>(data, data + size);
    }

    bool exists(std::string_view key) override {
        std::lock_guard<std::mutex> lock(mReadMutex);
        if (!mReadDb)
            return false;
        Stmt s;
        if (auto r = s.prepare(mReadDb, "SELECT 1 FROM blobs WHERE key = ?;"); !r) {
            logError("blobs.exists", r.error);
            return false;
        }
        sqlite3_bind_text(s.get(), 1, key.data(), static_cast<int>(key.size()), SQLITE_STATIC);
        return sqlite3_step(s.get()) == SQLITE_ROW;
    }

    std::vector<std::string> keys(std::string_view prefix) override {
        std::vector<std::string> out;
        std::lock_guard<std::mutex> lock(mReadMutex);
        if (!mReadDb)
            return out;
        Stmt s;
        // GLOB rather than LIKE: LIKE is case-insensitive for ASCII by default, and these keys are
        // namespaces ("campaign/<name>") where case is meaningful. The pattern is built from the
        // caller's prefix, so its wildcards are escaped -- a key containing '*' or '[' must not
        // silently widen someone else's query.
        if (auto r = s.prepare(mReadDb, "SELECT key FROM blobs WHERE key GLOB ? ORDER BY key;"); !r) {
            logError("blobs.keys", r.error);
            return out;
        }
        std::string pattern;
        pattern.reserve(prefix.size() + 8);
        for (char c : prefix) {
            if (c == '*' || c == '?' || c == '[' || c == ']') {
                pattern += '[';
                pattern += c;
                pattern += ']';
            } else {
                pattern += c;
            }
        }
        pattern += '*';
        sqlite3_bind_text(s.get(), 1, pattern.c_str(), static_cast<int>(pattern.size()), SQLITE_TRANSIENT);
        while (sqlite3_step(s.get()) == SQLITE_ROW) {
            const auto* text = sqlite3_column_text(s.get(), 0);
            if (text)
                out.emplace_back(reinterpret_cast<const char*>(text));
        }
        return out;
    }

    void put(std::string_view key, std::vector<std::byte> value) override {
        // Everything the task touches is copied here, on the caller's thread: it runs later, on
        // another thread, long after this frame is gone.
        mWriter.enqueue([this, k = std::string(key), v = std::move(value)]() -> Result {
            Stmt s;
            if (auto r = s.prepare(mWriteDb, "INSERT INTO blobs (key, value, updated_at) VALUES (?, ?, ?) "
                                             "ON CONFLICT(key) DO UPDATE SET value=excluded.value, "
                                             "updated_at=excluded.updated_at;");
                !r)
                return r;
            sqlite3_bind_text(s.get(), 1, k.c_str(), static_cast<int>(k.size()), SQLITE_STATIC);
            // A zero-length blob must still bind as a blob, not as NULL: sqlite3_bind_blob with a
            // null pointer binds NULL, and the column is NOT NULL, so an empty save would fail the
            // constraint instead of round-tripping as empty.
            sqlite3_bind_blob(s.get(), 2, v.empty() ? "" : static_cast<const void*>(v.data()),
                              static_cast<int>(v.size()), SQLITE_STATIC);
            sqlite3_bind_int64(s.get(), 3, nowSeconds());
            if (sqlite3_step(s.get()) != SQLITE_DONE)
                return Result::failure("blobs.put '" + k + "': " + sqlite3_errmsg(mWriteDb));
            return Result::success();
        });
    }

    void remove(std::string_view key) override {
        mWriter.enqueue([this, k = std::string(key)]() -> Result {
            Stmt s;
            if (auto r = s.prepare(mWriteDb, "DELETE FROM blobs WHERE key = ?;"); !r)
                return r;
            sqlite3_bind_text(s.get(), 1, k.c_str(), static_cast<int>(k.size()), SQLITE_STATIC);
            if (sqlite3_step(s.get()) != SQLITE_DONE)
                return Result::failure("blobs.remove '" + k + "': " + sqlite3_errmsg(mWriteDb));
            return Result::success();
        });
    }

    void logError(const char* what, const std::string& detail) const {
        if (!mLog)
            return;
        char buf[512];
        std::snprintf(buf, sizeof(buf), "persistence: %s failed: %s", what, detail.c_str());
        mLog->log(LogLevel::Error, __FILE__, __LINE__, buf);
    }

    std::string mPath;
    sqlite3* mWriteDb{nullptr}; // writer thread only, after startWriter()
    sqlite3* mReadDb{nullptr};  // any thread, under mReadMutex
    mutable std::mutex mReadMutex;
    AsyncWriter mWriter;
    ILogger* mLog{nullptr};
    int mSchemaVersion{0};
};

} // namespace

std::unique_ptr<IPersistence> openSqliteStore(const SqliteOptions& options, ILogger* log, std::string& error) {
    if (options.path.empty()) {
        error = "[persistence] sqlite_path is empty";
        return nullptr;
    }
    // An in-memory database is refused rather than quietly accepted. Every connection to
    // ":memory:" is its OWN empty database, so the two-connection design above would give a store
    // whose reads never see its writes -- and even if it worked, a persistence store that
    // persists nothing is a configuration nobody means to have.
    if (options.path == ":memory:" || options.path.rfind("file::memory:", 0) == 0) {
        error = "[persistence] sqlite_path may not be an in-memory database -- it would persist nothing";
        return nullptr;
    }

    // Create the parent directory: cache/ is gitignored and absent in a fresh checkout, and the
    // campaign saves that live beside this file create it the same way.
    const std::filesystem::path path(options.path);
    if (path.has_parent_path() && !path.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "creating " + path.parent_path().string() + ": " + ec.message();
            return nullptr;
        }
    }

    // Every failure below names the PATH. SQLite's own messages do not -- a read-only directory
    // reports "attempt to write a readonly database" and nothing else, which tells an operator
    // running several servers nothing about WHICH database. The operator's next action is to fix a
    // permission on a specific file, so the message has to say which file.
    const auto withPath = [&options](const std::string& detail) { return options.path + ": " + detail; };

    auto store = std::make_unique<SqliteStore>(options.writeQueueMax, log);
    if (auto r = store->openWriteConnection(options); !r) {
        error = withPath(r.error);
        return nullptr;
    }
    // Migrations run here, synchronously, before the writer thread exists -- so nothing else can
    // be touching the connection while the schema changes under it.
    if (auto r = store->migrate(); !r) {
        error = withPath(r.error);
        return nullptr;
    }
    // Only now: the file certainly exists and is at head, so a READONLY handle can attach.
    if (auto r = store->openReadConnection(options); !r) {
        error = withPath(r.error);
        return nullptr;
    }
    store->startWriter();

    if (log) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "persistence: sqlite store open at %s (schema v%d)", options.path.c_str(),
                      store->health().schemaVersion);
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }
    return store;
}

} // namespace fl::persist
