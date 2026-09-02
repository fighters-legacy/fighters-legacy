// SPDX-License-Identifier: GPL-3.0-or-later
// The server persistence store (#533, D24).
//
// What these pin is the machinery, not a schema: the migration runner's three outcomes, the async
// writer's two promises (nothing is dropped, flush() means durable), and the blob repository's
// round trip through a real SQLite file. #534 adds the account/stats/ban cases on top of it.
//
// Every case uses a real temporary FILE rather than an in-memory database, and that is deliberate
// twice over: the store refuses ":memory:" by design (a persistence store that persists nothing is
// a configuration nobody means to have), and a test that never touches a filesystem would not
// exercise WAL, the busy timeout, or the create-the-parent-directory path -- which is most of what
// can actually go wrong on an operator's machine.
#include "AsyncWriter.h"
#include "IPersistence.h"
#include "Migrations.h"
#include "NullStore.h"
#include "PostgresStore.h"
#include "SqliteStore.h"

#include "mock_log.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace fl;
using namespace fl::persist;

namespace {

// A temp directory that removes itself, so a failing assertion cannot leave a stray database behind
// for the next run to find and silently reuse.
struct TempDir {
    std::filesystem::path path;

    TempDir() {
        // Unique without <unistd.h>: these tests build on Windows too, and getpid() is not portable.
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("fl-persist-test-" + std::to_string(stamp) + "-" + std::to_string(counter++));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    [[nodiscard]] std::string db(const char* name = "test.db") const {
        return (path / name).string();
    }
};

std::vector<std::byte> bytes(std::string_view s) {
    std::vector<std::byte> v;
    v.reserve(s.size());
    for (char c : s)
        v.push_back(static_cast<std::byte>(c));
    return v;
}

std::string text(const std::vector<std::byte>& v) {
    std::string s;
    s.reserve(v.size());
    for (std::byte b : v)
        s.push_back(static_cast<char>(b));
    return s;
}

// True when the environment insists this build must actually reach a PostgreSQL server -- set by
// the CI lane whose entire purpose is to do so.
bool requirePostgres() {
    const char* v = std::getenv("FL_TEST_POSTGRES_REQUIRED");
    return v && *v && std::string_view(v) != "0";
}

std::unique_ptr<IPersistence> openAt(const std::string& path, ILogger* log, std::string& error) {
    SqliteOptions opts;
    opts.path = path;
    return openSqliteStore(opts, log, error);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Migration runner
// ---------------------------------------------------------------------------------------------

TEST_CASE("both backends' migration sets end at the same head version", "[persistence]") {
    // The point of the assertion is the SECOND backend: a migration added to SQLite and forgotten
    // in the Postgres set would otherwise be found by whichever operator ran Postgres first.
    REQUIRE_FALSE(sqliteMigrations().empty());
    REQUIRE_FALSE(postgresMigrations().empty());
    CHECK(sqliteMigrations().back().version == kSchemaHeadVersion);
    CHECK(postgresMigrations().back().version == kSchemaHeadVersion);

    // Versions are dense and 1-based in both sets: a gap would make "apply everything above the
    // current version" quietly skip a step on a store that stopped in the gap.
    for (auto set : {sqliteMigrations(), postgresMigrations()}) {
        int expected = 1;
        for (const auto& m : set) {
            CHECK(m.version == expected);
            CHECK(m.name != nullptr);
            CHECK(m.sql != nullptr);
            ++expected;
        }
    }
}

TEST_CASE("a fresh store migrates to head, and re-opening it changes nothing", "[persistence]") {
    TempDir dir;
    RecordingLogger log;
    std::string error;

    {
        auto store = openAt(dir.db(), &log, error);
        REQUIRE(store);
        CHECK(error.empty());
        CHECK(store->health().schemaVersion == kSchemaHeadVersion);
        CHECK(store->backendName() == "sqlite");
    }
    const int appliedFirstTime = log.count(LogLevel::Info, "applied migration");
    CHECK(appliedFirstTime == static_cast<int>(sqliteMigrations().size()));

    // Second open: idempotent. Nothing is applied, and the store still reports head.
    {
        auto store = openAt(dir.db(), &log, error);
        REQUIRE(store);
        CHECK(store->health().schemaVersion == kSchemaHeadVersion);
    }
    CHECK(log.count(LogLevel::Info, "applied migration") == appliedFirstTime);
    CHECK(log.count(LogLevel::Info, "up to date") == 1);
}

TEST_CASE("a store migrated by a newer build is refused, not opened", "[persistence]") {
    // The case that matters: an operator downgrades fl-server after an upgrade wrote a schema this
    // binary does not know. Carrying on would have it write rows against a shape it is guessing at,
    // and "migrating down" would delete the newer build's data. Refusing is the only honest answer,
    // so it is pinned here rather than left to the runner's comment.
    TempDir dir;
    RecordingLogger log;
    std::string error;
    {
        auto store = openAt(dir.db(), &log, error);
        REQUIRE(store);
    }

    // Forge a future version row through a fresh, minimal target rather than reaching into the
    // store: the runner's contract is what is under test, not SQLite.
    struct FakeTarget final : IMigrationTarget {
        int version{kSchemaHeadVersion + 5};
        std::vector<std::string> executed;

        Result exec(const char* sql) override {
            executed.emplace_back(sql);
            return Result::success();
        }
        Result currentVersion(int& out) override {
            out = version;
            return Result::success();
        }
        Result recordVersion(int, const char*) override {
            return Result::success();
        }
        [[nodiscard]] const char* versionTableDdl() const override {
            return "CREATE TABLE IF NOT EXISTS schema_version (version INTEGER);";
        }
    } target;

    const Result r = runMigrations(target, sqliteMigrations(), &log);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("NEWER fl-server") != std::string::npos);
    // Both versions appear, so the operator knows which binary to go back to.
    CHECK(r.error.find(std::to_string(kSchemaHeadVersion + 5)) != std::string::npos);
    // Nothing beyond creating the bookkeeping table was run: no BEGIN, no DDL.
    CHECK(target.executed.size() == 1);
}

TEST_CASE("a failing migration rolls back and reports which step failed", "[persistence]") {
    RecordingLogger log;
    struct FailingTarget final : IMigrationTarget {
        std::vector<std::string> executed;
        Result exec(const char* sql) override {
            executed.emplace_back(sql);
            const std::string s(sql);
            if (s.find("CREATE TABLE IF NOT EXISTS blobs") != std::string::npos)
                return Result::failure("disk full");
            return Result::success();
        }
        Result currentVersion(int& out) override {
            out = 0;
            return Result::success();
        }
        Result recordVersion(int, const char*) override {
            return Result::success();
        }
        [[nodiscard]] const char* versionTableDdl() const override {
            return "CREATE TABLE IF NOT EXISTS schema_version (version INTEGER);";
        }
    } target;

    const Result r = runMigrations(target, sqliteMigrations(), &log);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("migration 1") != std::string::npos);
    CHECK(r.error.find("disk full") != std::string::npos);
    // A ROLLBACK was issued, and no COMMIT: a half-applied set must leave the store at the last
    // COMPLETE version rather than in a state no migration describes.
    bool rolledBack = false;
    bool committed = false;
    for (const auto& s : target.executed) {
        if (s == "ROLLBACK;")
            rolledBack = true;
        if (s == "COMMIT;")
            committed = true;
    }
    CHECK(rolledBack);
    CHECK_FALSE(committed);
}

// ---------------------------------------------------------------------------------------------
// Blob repository
// ---------------------------------------------------------------------------------------------

TEST_CASE("blobs round-trip through a real file, and survive closing the store", "[persistence]") {
    TempDir dir;
    NullLogger log;
    std::string error;

    {
        auto store = openAt(dir.db(), &log, error);
        REQUIRE(store);
        store->blobs().put("campaign/desert", bytes("sortie 3"));
        // The write is QUEUED, not durable, until flush() -- that is the contract, and the
        // shutdown path depends on it.
        REQUIRE(store->flush().ok);
        store->close();
    }

    // Reopen: this is the acceptance bullet in miniature -- a process that went away and came back
    // sees what it wrote.
    auto store = openAt(dir.db(), &log, error);
    REQUIRE(store);
    auto value = store->blobs().get("campaign/desert");
    REQUIRE(value.has_value());
    CHECK(text(*value) == "sortie 3");
    CHECK(store->blobs().exists("campaign/desert"));
    CHECK_FALSE(store->blobs().exists("campaign/absent"));
    CHECK_FALSE(store->blobs().get("campaign/absent").has_value());
}

TEST_CASE("putting an existing key replaces it", "[persistence]") {
    TempDir dir;
    NullLogger log;
    std::string error;
    auto store = openAt(dir.db(), &log, error);
    REQUIRE(store);

    store->blobs().put("k", bytes("first"));
    store->blobs().put("k", bytes("second"));
    REQUIRE(store->flush().ok);

    auto value = store->blobs().get("k");
    REQUIRE(value.has_value());
    CHECK(text(*value) == "second");
    CHECK(store->blobs().keys("").size() == 1);
}

TEST_CASE("an empty blob round-trips as empty, not as absent", "[persistence]") {
    // sqlite3_bind_blob with a null pointer binds NULL, and the column is NOT NULL -- so an empty
    // payload is the one that turns a working save into a constraint violation. A campaign with
    // nothing yet recorded is exactly that payload.
    TempDir dir;
    NullLogger log;
    std::string error;
    auto store = openAt(dir.db(), &log, error);
    REQUIRE(store);

    store->blobs().put("empty", {});
    REQUIRE(store->flush().ok);
    CHECK(store->health().writesFailed == 0);

    auto value = store->blobs().get("empty");
    REQUIRE(value.has_value());
    CHECK(value->empty());
    CHECK(store->blobs().exists("empty"));
}

TEST_CASE("keys() filters by prefix, sorts, and does not let a key widen the query", "[persistence]") {
    TempDir dir;
    NullLogger log;
    std::string error;
    auto store = openAt(dir.db(), &log, error);
    REQUIRE(store);

    store->blobs().put("campaign/b", bytes("2"));
    store->blobs().put("campaign/a", bytes("1"));
    store->blobs().put("bans/legacy", bytes("x"));
    // A key holding a GLOB metacharacter. Passed as a prefix it must match only itself, not act as
    // a wildcard over the rest of the table.
    store->blobs().put("weird[a-z]/one", bytes("w"));
    REQUIRE(store->flush().ok);

    const auto campaign = store->blobs().keys("campaign/");
    REQUIRE(campaign.size() == 2);
    CHECK(campaign[0] == "campaign/a"); // sorted
    CHECK(campaign[1] == "campaign/b");

    CHECK(store->blobs().keys("").size() == 4);
    CHECK(store->blobs().keys("nothing/").empty());

    const auto weird = store->blobs().keys("weird[a-z]/");
    REQUIRE(weird.size() == 1);
    CHECK(weird[0] == "weird[a-z]/one");
}

TEST_CASE("remove() deletes, and removing an absent key is not an error", "[persistence]") {
    TempDir dir;
    NullLogger log;
    std::string error;
    auto store = openAt(dir.db(), &log, error);
    REQUIRE(store);

    store->blobs().put("k", bytes("v"));
    store->blobs().remove("k");
    store->blobs().remove("never-existed");
    REQUIRE(store->flush().ok);

    CHECK_FALSE(store->blobs().exists("k"));
    CHECK(store->health().writesFailed == 0);
}

// ---------------------------------------------------------------------------------------------
// Open-time refusals
// ---------------------------------------------------------------------------------------------

TEST_CASE("an in-memory path is refused with a reason, not opened", "[persistence]") {
    NullLogger log;
    std::string error;
    SqliteOptions opts;

    opts.path = ":memory:";
    CHECK(openSqliteStore(opts, &log, error) == nullptr);
    CHECK(error.find("persist nothing") != std::string::npos);

    opts.path = "file::memory:?cache=shared";
    CHECK(openSqliteStore(opts, &log, error) == nullptr);
    CHECK_FALSE(error.empty());

    opts.path = "";
    CHECK(openSqliteStore(opts, &log, error) == nullptr);
    CHECK(error.find("sqlite_path") != std::string::npos);
}

TEST_CASE("the parent directory is created, because cache/ is absent in a fresh checkout", "[persistence]") {
    TempDir dir;
    NullLogger log;
    std::string error;
    const auto nested = (dir.path / "a" / "b" / "fl-server.db").string();

    auto store = openSqliteStore(SqliteOptions{nested, 5000, 64}, &log, error);
    REQUIRE(store);
    CHECK(std::filesystem::exists(nested));
}

TEST_CASE("an unopenable path fails with the path in the message", "[persistence]") {
    // What ServerRuntime turns into a refusal to start. The message has to name the path, because
    // the operator's next action is to fix a permission on it.
    TempDir dir;
    NullLogger log;
    std::string error;
    const auto readOnly = dir.path / "ro";
    std::filesystem::create_directories(readOnly);
    std::filesystem::permissions(readOnly, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);

    const auto target = (readOnly / "fl-server.db").string();
    auto store = openSqliteStore(SqliteOptions{target, 5000, 64}, &log, error);
    // Running as root defeats a read-only directory. Passing quietly in that case would make the
    // case decorative, so it says which happened instead.
    if (store) {
        std::filesystem::permissions(readOnly, std::filesystem::perms::owner_all);
        SKIP("the filesystem allowed the write (running as root?) -- nothing to assert");
    }
    // SQLite's own text is "attempt to write a readonly database", which names nothing. The path is
    // the point of the message: the operator's next action is to fix a permission on ONE file, and
    // a host running several servers would otherwise be told only that one of them failed.
    CHECK(error.find(target) != std::string::npos);

    std::filesystem::permissions(readOnly, std::filesystem::perms::owner_all);
}

TEST_CASE("several stores open the same database without any of them refusing to start", "[persistence]") {
    // The test suite itself is this scenario: ctest runs in parallel, several of its cases spawn a
    // real fl-server, and they share a working directory -- so they share cache/fl-server.db. A
    // developer running two servers from one directory is the same shape.
    //
    // The failure this guards is severe out of all proportion to its cause: the delete->WAL journal
    // transition needs a brief exclusive lock and does NOT go through the busy handler, so a store
    // that treated it as fatal would refuse to start a perfectly good server because another one
    // happened to be opening at the same moment.
    TempDir dir;
    NullLogger log;
    const std::string path = dir.db("shared.db");

    std::vector<std::unique_ptr<IPersistence>> stores;
    std::vector<std::string> errors(6);
    for (std::size_t i = 0; i < 6; ++i) {
        stores.push_back(openSqliteStore(SqliteOptions{path, 5000, 64}, &log, errors[i]));
        INFO("store " << i << ": " << errors[i]);
        REQUIRE(stores.back() != nullptr);
        CHECK(stores.back()->health().schemaVersion == kSchemaHeadVersion);
    }

    // And they genuinely share one database: what one writes, another reads.
    stores[0]->blobs().put("shared/key", bytes("written by the first"));
    REQUIRE(stores[0]->flush().ok);
    auto seen = stores[5]->blobs().get("shared/key");
    REQUIRE(seen.has_value());
    CHECK(text(*seen) == "written by the first");
}

// ---------------------------------------------------------------------------------------------
// Async writer
// ---------------------------------------------------------------------------------------------

TEST_CASE("the writer applies tasks in order and flush() waits for the last one", "[persistence]") {
    NullLogger log;
    AsyncWriter writer(16, &log);
    writer.start();

    std::vector<int> applied;
    std::mutex mutex;
    for (int i = 0; i < 8; ++i) {
        writer.enqueue([&, i]() -> Result {
            // A task that takes real time: flush() must wait for the RUNNING task, not merely for
            // an empty queue. Waiting on the queue alone returns while the last write is still in
            // flight, which is exactly when a shutdown path would then close the connection.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::lock_guard<std::mutex> lock(mutex);
            applied.push_back(i);
            return Result::success();
        });
    }
    CHECK(writer.flush().ok);

    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(applied.size() == 8);
    for (int i = 0; i < 8; ++i)
        CHECK(applied[static_cast<std::size_t>(i)] == i);
    CHECK(writer.stats().completed == 8);
    CHECK(writer.stats().depth == 0);
}

TEST_CASE("a full queue blocks the caller; it never drops a write", "[persistence]") {
    // The property the whole design exists for. A dropped ban is a ban that silently did not
    // happen, and an operator has no way to find out. Blocking a main-thread caller briefly on an
    // already-overloaded server is the cost that buys "every accepted write is applied".
    NullLogger log;
    AsyncWriter writer(2, &log); // cap of 2, so 32 enqueues must block repeatedly
    writer.start();

    std::atomic<int> ran{0};
    for (int i = 0; i < 32; ++i) {
        writer.enqueue([&]() -> Result {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            ++ran;
            return Result::success();
        });
    }
    CHECK(writer.flush().ok);

    const auto stats = writer.stats();
    CHECK(ran.load() == 32);
    CHECK(stats.enqueued == 32);
    CHECK(stats.completed == 32);
    CHECK(stats.failed == 0);
    // The high-water mark is the honest capacity signal -- a depth sampled after the drain would
    // read zero and tell an operator nothing.
    CHECK(stats.highWater > 0);
    CHECK(stats.highWater <= 2);
}

TEST_CASE("a failed write is counted, logged and reported by the next flush", "[persistence]") {
    // Errors are never swallowed: an async write cannot answer its caller, so the failure has to
    // surface as a count an operator can see and a line they can read.
    RecordingLogger log;
    AsyncWriter writer(8, &log);
    writer.start();

    writer.enqueue([]() -> Result { return Result::success(); });
    writer.enqueue([]() -> Result { return Result::failure("no space left on device"); });

    const Result flushed = writer.flush();
    CHECK_FALSE(flushed.ok);
    CHECK(flushed.error.find("no space left") != std::string::npos);
    CHECK(log.count(LogLevel::Error, "write failed") == 1);

    const auto stats = writer.stats();
    CHECK(stats.completed == 1);
    CHECK(stats.failed == 1);
    CHECK(stats.lastError.find("no space left") != std::string::npos);

    // Each flush reports ITS OWN window. Returning the same error forever would make the second
    // flush look like a second failure.
    CHECK(writer.flush().ok);
    // The operator-facing record is not cleared, though: writesFailed and lastError persist.
    CHECK(writer.stats().failed == 1);
    CHECK_FALSE(writer.stats().lastError.empty());
}

TEST_CASE("stop() drains before it stops", "[persistence]") {
    // Shutdown is precisely when the queued writes matter most, so stopping must not discard them.
    NullLogger log;
    std::atomic<int> ran{0};
    {
        AsyncWriter writer(64, &log);
        writer.start();
        for (int i = 0; i < 24; ++i)
            writer.enqueue([&]() -> Result {
                ++ran;
                return Result::success();
            });
        writer.stop();
        CHECK(ran.load() == 24);
        writer.stop(); // idempotent
    }
    CHECK(ran.load() == 24);
}

TEST_CASE("a closed store says so rather than silently accepting writes", "[persistence]") {
    RecordingLogger log;
    AsyncWriter writer(4, &log);
    writer.start();
    writer.stop();

    std::atomic<int> ran{0};
    writer.enqueue([&]() -> Result {
        ++ran;
        return Result::success();
    });
    CHECK(ran.load() == 0);
    CHECK(log.count(LogLevel::Warn, "the store is closed") == 1);
}

// ---------------------------------------------------------------------------------------------
// Null store
// ---------------------------------------------------------------------------------------------

TEST_CASE("the null store is inert and says which backend it is", "[persistence]") {
    auto store = makeNullStore();
    REQUIRE(store);
    CHECK(store->backendName() == "null");
    CHECK_FALSE(store->health().open);

    store->blobs().put("k", bytes("v"));
    CHECK(store->flush().ok);
    CHECK_FALSE(store->blobs().get("k").has_value());
    CHECK_FALSE(store->blobs().exists("k"));
    CHECK(store->blobs().keys("").empty());
    store->blobs().remove("k");
    store->close();
    CHECK(store->health().writesFailed == 0);
}

// ---------------------------------------------------------------------------------------------
// PostgreSQL
// ---------------------------------------------------------------------------------------------

TEST_CASE("a DSN is redacted before it can reach a log line", "[persistence]") {
    // Compiled in every configuration on purpose: this is pure string handling, and a test that
    // only runs under FL_WITH_POSTGRES is a test that rots on the four platforms that never set it.
    CHECK(redactDsn("host=db user=fl password=hunter2 dbname=fl") == "host=db user=fl password=*** dbname=fl");
    CHECK(redactDsn("password='a b c' host=db") == "password=*** host=db");
    CHECK(redactDsn("password='it\\'s' host=db") == "password=*** host=db");
    CHECK(redactDsn("postgresql://fl:hunter2@db:5432/fighters") == "postgresql://***@db:5432/fighters");
    // Nothing secret, nothing changed.
    CHECK(redactDsn("host=db user=fl dbname=fl") == "host=db user=fl dbname=fl");
    CHECK(redactDsn("postgresql://db:5432/fighters") == "postgresql://db:5432/fighters");
    CHECK(redactDsn("").empty());
}

TEST_CASE("a build without the postgres backend refuses it as a BUILD problem", "[persistence]") {
    // The wrong message here costs an operator an afternoon: "PostgreSQL is unavailable" sends them
    // to check a server and a network for something that was never compiled.
    NullLogger log;
    std::string error;
    PostgresOptions opts;
    opts.dsn = "host=localhost dbname=fl";

    if (!postgresBackendAvailable()) {
        CHECK(openPostgresStore(opts, &log, error) == nullptr);
        CHECK(error.find("FL_WITH_POSTGRES") != std::string::npos);
    }

    opts.dsn.clear();
    CHECK(openPostgresStore(opts, &log, error) == nullptr);
    CHECK_FALSE(error.empty());
}

TEST_CASE("the postgres backend round-trips against a real server", "[persistence][postgres]") {
    // Runs against FL_TEST_POSTGRES_DSN, which the CI lane points at its service container.
    //
    // It SKIPS when the variable is absent (a developer's box) and it FAILS when the variable is
    // present and the store will not open. That asymmetry is the point: a test that quietly passed
    // whenever it could not connect would make the whole lane decorative -- CI would be green for
    // a backend nobody had run since the day it was written.
    // FL_TEST_POSTGRES_REQUIRED is the lane's assertion that postgres MUST work here. Without it a
    // missing DSN skips, which is right on a developer's box. With it, skipping is a failure --
    // otherwise the CI lane degrades to green-and-empty the moment its service container, its build
    // flag or its environment stops being wired up, and nobody finds out.
    const bool required = requirePostgres();
    const char* dsn = std::getenv("FL_TEST_POSTGRES_DSN");
    if (!dsn || !*dsn) {
        if (required)
            FAIL("FL_TEST_POSTGRES_REQUIRED is set but FL_TEST_POSTGRES_DSN is not -- this lane is "
                 "supposed to be testing against a real server and would otherwise pass having "
                 "tested nothing");
        SKIP("FL_TEST_POSTGRES_DSN is not set (the postgres CI lane sets it)");
    }
    REQUIRE(postgresBackendAvailable());

    RecordingLogger log;
    std::string error;
    PostgresOptions opts;
    opts.dsn = dsn;
    auto store = openPostgresStore(opts, &log, error);
    REQUIRE_FALSE(store == nullptr); // the DSN was given, so a failure here is a real failure
    CHECK(store->backendName() == "postgres");
    CHECK(store->health().schemaVersion == kSchemaHeadVersion);

    // The same behaviours the SQLite cases pin, against the other dialect: the two backends must
    // not drift into meaning different things.
    const std::string key =
        "test/roundtrip-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    store->blobs().put(key, bytes("postgres value"));
    store->blobs().put(key + "/empty", {});
    REQUIRE(store->flush().ok);
    CHECK(store->health().writesFailed == 0);

    auto value = store->blobs().get(key);
    REQUIRE(value.has_value());
    CHECK(text(*value) == "postgres value");

    auto empty = store->blobs().get(key + "/empty");
    REQUIRE(empty.has_value());
    CHECK(empty->empty());

    CHECK(store->blobs().exists(key));
    const auto listed = store->blobs().keys(key);
    CHECK(listed.size() == 2);

    store->blobs().remove(key);
    store->blobs().remove(key + "/empty");
    REQUIRE(store->flush().ok);
    CHECK_FALSE(store->blobs().exists(key));

    // Re-opening finds the schema already at head and applies nothing (the migration runner is
    // shared, but its per-backend bookkeeping SQL is not).
    store->close();
    auto reopened = openPostgresStore(opts, &log, error);
    REQUIRE(reopened);
    CHECK(reopened->health().schemaVersion == kSchemaHeadVersion);
    CHECK(log.count(LogLevel::Info, "up to date") >= 1);
}

TEST_CASE("a bad postgres DSN fails to open with a reason", "[persistence][postgres]") {
    if (!postgresBackendAvailable()) {
        if (requirePostgres())
            FAIL("FL_TEST_POSTGRES_REQUIRED is set but this binary was built without "
                 "FL_WITH_POSTGRES -- the lane is configured wrong");
        SKIP("built without FL_WITH_POSTGRES");
    }

    NullLogger log;
    std::string error;
    PostgresOptions opts;
    // A port nothing listens on: the refusal must carry libpq's reason, not an empty string that
    // ServerRuntime would then print as "cannot open the postgres store: ".
    opts.dsn = "postgresql://fl:fl@127.0.0.1:1/fl_test?connect_timeout=2";
    CHECK(openPostgresStore(opts, &log, error) == nullptr);
    CHECK_FALSE(error.empty());
}
