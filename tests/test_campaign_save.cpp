// SPDX-License-Identifier: GPL-3.0-or-later
// Where a campaign's saved war comes from, and where it goes (#534, D25).
//
// D25 absorbs `cache/campaign_<name>.flsave` into the store as a blob. That touches something
// operators already have on disk, so the precedence rules matter more than the mechanism:
//
//   * a war that predates the store must survive the upgrade,
//   * a server with persistence disabled must keep working exactly as it did,
//   * and a store that has taken over must NEVER rewind to the stale file left beside it.
//
// The last one is the reason the file is not deleted on import, and therefore the reason it could
// come back to haunt a server. It is pinned here rather than left to a campaign run nobody automates.
#include "CampaignSave.h"

#include <IPersistence.h>
#include <NullStore.h>
#include <SqliteStore.h>

#include "mock_log.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace fl;

namespace {

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("fl-campaign-save-" + std::to_string(stamp) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    [[nodiscard]] std::string file(const char* name) const {
        return (path / name).string();
    }
};

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::unique_ptr<persist::IPersistence> openStore(const TempDir& dir, ILogger* log) {
    std::string error;
    persist::SqliteOptions opts;
    opts.path = dir.file("fl-server.db");
    auto store = persist::openSqliteStore(opts, log, error);
    INFO("open error: " << error);
    REQUIRE(store);
    return store;
}

} // namespace

TEST_CASE("campaign save: nothing anywhere loads as nothing", "[campaign_save]") {
    TempDir dir;
    NullLogger log;
    auto store = openStore(dir, &log);

    const auto loaded = loadCampaignSave({store.get(), "campaign/desert", dir.file("absent.flsave")}, &log);
    CHECK(loaded.blob.empty());
    CHECK_FALSE(loaded.fromStore);
    CHECK_FALSE(loaded.importedFile);
}

TEST_CASE("campaign save: a war round-trips through the store", "[campaign_save]") {
    TempDir dir;
    NullLogger log;
    auto store = openStore(dir, &log);
    const CampaignSaveIo io{store.get(), "campaign/desert", dir.file("campaign_desert.flsave")};

    saveCampaignSave(io, "sortie 3, frontline advanced", &log);
    REQUIRE(store->flush().ok);

    const auto loaded = loadCampaignSave(io, &log);
    CHECK(loaded.blob == "sortie 3, frontline advanced");
    CHECK(loaded.fromStore);
    // The store took it, so nothing was written beside it.
    CHECK_FALSE(std::filesystem::exists(io.filePath));
}

TEST_CASE("campaign save: a pre-store file is imported once and left on disk", "[campaign_save]") {
    // The upgrade path for every server that has been running a campaign since before #534.
    TempDir dir;
    RecordingLogger log;
    auto store = openStore(dir, &log);
    const CampaignSaveIo io{store.get(), "campaign/desert", dir.file("campaign_desert.flsave")};
    writeFile(io.filePath, "a war in progress");

    const auto first = loadCampaignSave(io, &log);
    CHECK(first.blob == "a war in progress");
    CHECK_FALSE(first.fromStore);
    CHECK(first.importedFile);
    CHECK(log.count(LogLevel::Info, "imported") == 1);
    // The file is NOT deleted: it is the operator's record, and a downgrade must still find it.
    CHECK(std::filesystem::exists(io.filePath));
    CHECK(readFile(io.filePath) == "a war in progress");

    REQUIRE(store->flush().ok);

    // Second load comes from the store, and does not import again.
    const auto second = loadCampaignSave(io, &log);
    CHECK(second.blob == "a war in progress");
    CHECK(second.fromStore);
    CHECK_FALSE(second.importedFile);
    CHECK(log.count(LogLevel::Info, "imported") == 1);
}

TEST_CASE("campaign save: the store WINS over a stale file left beside it", "[campaign_save]") {
    // ⚑ The reason the precedence order is store-first, and the failure the import's leave-the-file
    // policy would otherwise create. After an import the file stays on disk frozen at the moment of
    // the upgrade, while the store advances every sortie. A file-first load would silently rewind an
    // operator's war by however long they have been playing since -- and it would look like the
    // campaign simply forgot, with nothing in the log to explain it.
    TempDir dir;
    NullLogger log;
    auto store = openStore(dir, &log);
    const CampaignSaveIo io{store.get(), "campaign/desert", dir.file("campaign_desert.flsave")};

    writeFile(io.filePath, "WEEKS AGO");
    saveCampaignSave(io, "last night", &log);
    REQUIRE(store->flush().ok);

    const auto loaded = loadCampaignSave(io, &log);
    CHECK(loaded.blob == "last night");
    CHECK(loaded.fromStore);
}

TEST_CASE("campaign save: with persistence disabled the file is still the record", "[campaign_save]") {
    // Turning the store off must not cost an operator their campaign. The null store reads back
    // nothing and swallows writes, so treating it as usable would silently drop every save.
    TempDir dir;
    NullLogger log;
    auto store = persist::makeNullStore();
    const CampaignSaveIo io{store.get(), "campaign/desert", dir.file("campaign_desert.flsave")};

    saveCampaignSave(io, "flown offline", &log);
    REQUIRE(std::filesystem::exists(io.filePath));
    CHECK(readFile(io.filePath) == "flown offline");

    const auto loaded = loadCampaignSave(io, &log);
    CHECK(loaded.blob == "flown offline");
    CHECK_FALSE(loaded.fromStore);
    // Nothing was imported: there was no store to import into.
    CHECK_FALSE(loaded.importedFile);
}

TEST_CASE("campaign save: no store at all behaves like persistence disabled", "[campaign_save]") {
    TempDir dir;
    NullLogger log;
    const CampaignSaveIo io{nullptr, "campaign/desert", dir.file("campaign_desert.flsave")};

    saveCampaignSave(io, "no store here", &log);
    CHECK(readFile(io.filePath) == "no store here");
    const auto loaded = loadCampaignSave(io, &log);
    CHECK(loaded.blob == "no store here");
    CHECK_FALSE(loaded.fromStore);
}

TEST_CASE("campaign save: a later save replaces the earlier war, not appends to it", "[campaign_save]") {
    TempDir dir;
    NullLogger log;
    auto store = openStore(dir, &log);
    const CampaignSaveIo io{store.get(), "campaign/desert", dir.file("campaign_desert.flsave")};

    saveCampaignSave(io, "sortie 1", &log);
    saveCampaignSave(io, "sortie 2", &log);
    REQUIRE(store->flush().ok);
    CHECK(loadCampaignSave(io, &log).blob == "sortie 2");
}
