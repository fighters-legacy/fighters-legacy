// SPDX-License-Identifier: GPL-3.0-or-later
// [persistence] -> a store, or a refusal to start (#533).
//
// This is the decision that determines whether fl-server boots at all, so its four outcomes are
// pinned here rather than left to whichever operator hits them first. It used to be a private method
// on ServerRuntime::Impl, reachable only by running a whole server, which meant the postgres branch
// and the refusal path were exercised by nothing at all.
#include "PersistenceSetup.h"

#include <IPersistence.h>

#include "mock_log.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

using namespace fl;

namespace {

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("fl-persist-setup-" + std::to_string(stamp) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("openConfiguredStore: the default config opens a working sqlite store", "[persistence_setup]") {
    TempDir dir;
    RecordingLogger log;
    ServerConfig cfg;
    cfg.persistence.sqlitePath = (dir.path / "fl-server.db").string();

    auto opened = openConfiguredStore(cfg, &log);
    REQUIRE(opened.ok());
    CHECK(opened.error.empty());
    CHECK(opened.store->backendName() == "sqlite");
    CHECK(opened.store->health().open);
    opened.store->close();
}

TEST_CASE("openConfiguredStore: enabled = false is a SUCCESS that persists nothing", "[persistence_setup]") {
    // Not a failure. The operator asked for no persistence and got exactly that -- so the server
    // starts, and the log says plainly what was given up, because the alternative is an operator who
    // believes their bans are durable.
    RecordingLogger log;
    ServerConfig cfg;
    cfg.persistence.enabled = false;
    cfg.persistence.sqlitePath = "/definitely/not/writable/fl-server.db";

    auto opened = openConfiguredStore(cfg, &log);
    REQUIRE(opened.ok());
    CHECK(opened.error.empty());
    CHECK(opened.store->backendName() == "null");
    CHECK_FALSE(opened.store->health().open);
    CHECK(log.count(LogLevel::Info, "will NOT survive a restart") == 1);
}

TEST_CASE("openConfiguredStore: an unopenable store refuses, with the whole message", "[persistence_setup]") {
    // The last thing an operator sees before the process exits. Each clause earns its place: the
    // backend (which store), the underlying reason and path (what to fix), and the escape hatch
    // (how to proceed deliberately without one).
    RecordingLogger log;
    ServerConfig cfg;
    cfg.persistence.sqlitePath = ":memory:"; // refused by the backend: it would persist nothing

    auto opened = openConfiguredStore(cfg, &log);
    REQUIRE_FALSE(opened.ok());
    CHECK(opened.store == nullptr);
    CHECK(opened.error.find("sqlite") != std::string::npos);
    CHECK(opened.error.find("persist nothing") != std::string::npos);
    CHECK(opened.error.find("enabled = false") != std::string::npos);
}

TEST_CASE("openConfiguredStore: backend = postgres reaches the postgres backend", "[persistence_setup]") {
    // Whichever way this build was configured, the branch is TAKEN and the refusal explains itself.
    // Without the postgres backend compiled in, the message must say so as a BUILD fact -- telling
    // an operator "PostgreSQL is unavailable" sends them to check a server that was never contacted.
    RecordingLogger log;
    ServerConfig cfg;
    cfg.persistence.backend = "postgres";
    cfg.persistence.postgresDsn = "postgresql://fl:fl@127.0.0.1:1/none?connect_timeout=2";

    auto opened = openConfiguredStore(cfg, &log);
    if (opened.ok()) {
        // Only reachable on an FL_WITH_POSTGRES build that somehow connected to port 1.
        CHECK(opened.store->backendName() == "postgres");
        opened.store->close();
    } else {
        CHECK(opened.error.find("postgres") != std::string::npos);
        CHECK(opened.error.find("enabled = false") != std::string::npos);
    }
}

TEST_CASE("openConfiguredStore: an empty postgres dsn is refused before any connection", "[persistence_setup]") {
    RecordingLogger log;
    ServerConfig cfg;
    cfg.persistence.backend = "postgres";
    cfg.persistence.postgresDsn.clear();

    auto opened = openConfiguredStore(cfg, &log);
    REQUIRE_FALSE(opened.ok());
    CHECK(opened.error.find("postgres") != std::string::npos);
}

TEST_CASE("openConfiguredStore: a null logger is tolerated on every path", "[persistence_setup]") {
    // The logger is documented optional across this codebase, and these paths run during early
    // startup where it is the thing most likely to be missing.
    TempDir dir;
    ServerConfig cfg;
    cfg.persistence.sqlitePath = (dir.path / "fl-server.db").string();
    auto opened = openConfiguredStore(cfg, nullptr);
    REQUIRE(opened.ok());
    opened.store->close();

    ServerConfig off;
    off.persistence.enabled = false;
    CHECK(openConfiguredStore(off, nullptr).ok());

    ServerConfig bad;
    bad.persistence.sqlitePath = ":memory:";
    CHECK_FALSE(openConfiguredStore(bad, nullptr).ok());
}
