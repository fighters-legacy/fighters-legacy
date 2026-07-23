// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "StdFilesystemWatcher.h"
#include "temp_path.h"

#include <chrono>
#include <fstream>
#include <string>
#include <vector>

using namespace fl;
namespace fs = std::filesystem;

namespace {
void writeFile(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content;
}
// Bump a file's mtime forward so a rewrite is a distinct signature regardless of filesystem mtime
// granularity (no sleeps — the whole point of the two-scan settle over a time debounce).
void touchLater(const fs::path& p, int secondsAhead) {
    std::error_code ec;
    fs::last_write_time(p, fs::file_time_type::clock::now() + std::chrono::seconds(secondsAhead), ec);
}
bool hasEvent(const std::vector<IFilesystemWatcher::Event>& evs, const std::string& path,
              IFilesystemWatcher::EventType type) {
    for (auto& e : evs)
        if (e.path == path && e.type == type)
            return true;
    return false;
}
} // namespace

TEST_CASE("StdFilesystemWatcher: create fires Created after two scans (settle)", "[watcher]") {
    test::TempDirGuard tmp{"fl-watcher"};
    fs::create_directories(tmp.path() / "aircraft");
    // pollIntervalMs = 0 -> rescan every poll. First poll snapshots the (empty) tree.
    StdFilesystemWatcher w(tmp.path(), tmp.path(), 0);
    REQUIRE(w.watch(PathDomain::Assets, "aircraft", true));

    writeFile(tmp.path() / "aircraft" / "f5e.glb", "abc");
    // First scan: candidate (pending). No event yet.
    auto e1 = w.pollEvents();
    CHECK_FALSE(hasEvent(e1, "aircraft/f5e.glb", IFilesystemWatcher::EventType::Created));
    // Second scan: signature stable -> Created.
    auto e2 = w.pollEvents();
    CHECK(hasEvent(e2, "aircraft/f5e.glb", IFilesystemWatcher::EventType::Created));
}

TEST_CASE("StdFilesystemWatcher: modify fires Modified after settle", "[watcher]") {
    test::TempDirGuard tmp{"fl-watcher"};
    writeFile(tmp.path() / "aircraft" / "f5e.glb", "abc");
    StdFilesystemWatcher w(tmp.path(), tmp.path(), 0);
    REQUIRE(w.watch(PathDomain::Assets, "aircraft", true)); // snapshots f5e.glb as known

    writeFile(tmp.path() / "aircraft" / "f5e.glb", "abcdef");
    touchLater(tmp.path() / "aircraft" / "f5e.glb", 10);
    w.pollEvents();           // candidate
    auto e2 = w.pollEvents(); // stable -> Modified
    CHECK(hasEvent(e2, "aircraft/f5e.glb", IFilesystemWatcher::EventType::Modified));
}

TEST_CASE("StdFilesystemWatcher: delete fires Deleted immediately", "[watcher]") {
    test::TempDirGuard tmp{"fl-watcher"};
    writeFile(tmp.path() / "aircraft" / "f5e.glb", "abc");
    StdFilesystemWatcher w(tmp.path(), tmp.path(), 0);
    REQUIRE(w.watch(PathDomain::Assets, "aircraft", true));

    std::error_code ec;
    fs::remove(tmp.path() / "aircraft" / "f5e.glb", ec);
    auto e = w.pollEvents(); // Deleted needs no settle
    CHECK(hasEvent(e, "aircraft/f5e.glb", IFilesystemWatcher::EventType::Deleted));
}

TEST_CASE("StdFilesystemWatcher: rename shows as Deleted + Created", "[watcher]") {
    test::TempDirGuard tmp{"fl-watcher"};
    writeFile(tmp.path() / "aircraft" / "old.glb", "abc");
    StdFilesystemWatcher w(tmp.path(), tmp.path(), 0);
    REQUIRE(w.watch(PathDomain::Assets, "aircraft", true));

    std::error_code ec;
    fs::rename(tmp.path() / "aircraft" / "old.glb", tmp.path() / "aircraft" / "new.glb", ec);
    auto e1 = w.pollEvents();
    CHECK(hasEvent(e1, "aircraft/old.glb", IFilesystemWatcher::EventType::Deleted)); // immediate
    auto e2 = w.pollEvents();
    CHECK(hasEvent(e2, "aircraft/new.glb", IFilesystemWatcher::EventType::Created)); // after settle
    // Renamed is never emitted.
    for (auto& e : e1)
        CHECK(e.type != IFilesystemWatcher::EventType::Renamed);
}

TEST_CASE("StdFilesystemWatcher: non-recursive ignores subdirectories", "[watcher]") {
    test::TempDirGuard tmp{"fl-watcher"};
    fs::create_directories(tmp.path() / "aircraft" / "f5e");
    StdFilesystemWatcher w(tmp.path(), tmp.path(), 0);
    REQUIRE(w.watch(PathDomain::Assets, "aircraft", /*recursive=*/false));

    writeFile(tmp.path() / "aircraft" / "f5e" / "f5e.glb", "abc");
    w.pollEvents();
    auto e = w.pollEvents();
    CHECK_FALSE(hasEvent(e, "aircraft/f5e/f5e.glb", IFilesystemWatcher::EventType::Created));
}

TEST_CASE("StdFilesystemWatcher: watch() fails on a nonexistent directory", "[watcher]") {
    test::TempDirGuard tmp{"fl-watcher"};
    StdFilesystemWatcher w(tmp.path(), tmp.path(), 0);
    CHECK_FALSE(w.watch(PathDomain::Assets, "does-not-exist", true));
}

TEST_CASE("StdFilesystemWatcher: unwatch stops events", "[watcher]") {
    test::TempDirGuard tmp{"fl-watcher"};
    fs::create_directories(tmp.path() / "aircraft");
    StdFilesystemWatcher w(tmp.path(), tmp.path(), 0);
    REQUIRE(w.watch(PathDomain::Assets, "aircraft", true));
    w.unwatch(PathDomain::Assets, "aircraft");

    writeFile(tmp.path() / "aircraft" / "f5e.glb", "abc");
    w.pollEvents();
    auto e = w.pollEvents();
    CHECK(e.empty());
}
