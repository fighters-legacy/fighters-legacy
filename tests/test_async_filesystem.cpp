// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "IAsyncFilesystem.h"
#include "mock_hal.h"

#include <cstdint>
#include <string>

using namespace fl;
#include <vector>

// Captures onReadComplete results for assertion.
struct Collector : IAsyncFilesystemHandler {
    struct Result {
        AsyncReadId id;
        AsyncReadStatus status;
        std::vector<uint8_t> data;
        std::string error;
    };
    std::vector<Result> results;

    void onReadComplete(AsyncReadId id, AsyncReadStatus st, const void* data, std::size_t n, const char* err) override {
        const auto* p = static_cast<const uint8_t*>(data);
        results.push_back(
            {id, st, (data && n > 0) ? std::vector<uint8_t>(p, p + n) : std::vector<uint8_t>{}, err ? err : ""});
    }
};

TEST_CASE("MockAsyncFilesystem readFileAsync returns nonzero id", "[async_fs]") {
    MockAsyncFilesystem fs;
    Collector col;
    fs.init();
    fs.addFile("terrain/tile0.bin", "data");
    AsyncReadId id = fs.readFileAsync(PathDomain::Assets, "terrain/tile0.bin", &col);
    REQUIRE(id > 0);
}

TEST_CASE("MockAsyncFilesystem id 0 is never returned", "[async_fs]") {
    MockAsyncFilesystem fs;
    Collector col;
    fs.init();
    fs.addFile("a.bin", "x");
    AsyncReadId id1 = fs.readFileAsync(PathDomain::Assets, "a.bin", &col);
    AsyncReadId id2 = fs.readFileAsync(PathDomain::Assets, "a.bin", &col);
    REQUIRE(id1 != 0);
    REQUIRE(id2 != 0);
    REQUIRE(id1 != id2);
}

TEST_CASE("MockAsyncFilesystem service fires Success with correct data", "[async_fs]") {
    MockAsyncFilesystem fs;
    Collector col;
    fs.init();
    fs.addFile("terrain/tile0.bin", "hello");

    AsyncReadId id = fs.readFileAsync(PathDomain::Assets, "terrain/tile0.bin", &col);
    fs.service();

    REQUIRE(col.results.size() == 1);
    REQUIRE(col.results[0].id == id);
    REQUIRE(col.results[0].status == AsyncReadStatus::Success);
    std::string got(col.results[0].data.begin(), col.results[0].data.end());
    REQUIRE(got == "hello");
    REQUIRE(col.results[0].error.empty());
}

TEST_CASE("MockAsyncFilesystem service fires Error for missing file", "[async_fs]") {
    MockAsyncFilesystem fs;
    Collector col;
    fs.init();

    AsyncReadId id = fs.readFileAsync(PathDomain::Assets, "missing.bin", &col);
    fs.service();

    REQUIRE(col.results.size() == 1);
    REQUIRE(col.results[0].id == id);
    REQUIRE(col.results[0].status == AsyncReadStatus::Error);
    REQUIRE(!col.results[0].error.empty());
}

TEST_CASE("MockAsyncFilesystem cancelRead before service fires Cancelled", "[async_fs]") {
    MockAsyncFilesystem fs;
    Collector col;
    fs.init();
    fs.addFile("terrain/tile0.bin", "data");

    AsyncReadId id = fs.readFileAsync(PathDomain::Assets, "terrain/tile0.bin", &col);
    fs.cancelRead(id);
    fs.service();

    REQUIRE(col.results.size() == 1);
    REQUIRE(col.results[0].id == id);
    REQUIRE(col.results[0].status == AsyncReadStatus::Cancelled);
}

TEST_CASE("MockAsyncFilesystem shutdown cancels all pending requests", "[async_fs]") {
    MockAsyncFilesystem fs;
    Collector col;
    fs.init();
    fs.addFile("a.bin", "a");
    fs.addFile("b.bin", "b");

    AsyncReadId id1 = fs.readFileAsync(PathDomain::Assets, "a.bin", &col);
    AsyncReadId id2 = fs.readFileAsync(PathDomain::Assets, "b.bin", &col);
    fs.shutdown();

    REQUIRE(col.results.size() == 2);
    bool saw1 = false, saw2 = false;
    for (auto& r : col.results) {
        REQUIRE(r.status == AsyncReadStatus::Cancelled);
        if (r.id == id1)
            saw1 = true;
        if (r.id == id2)
            saw2 = true;
    }
    REQUIRE(saw1);
    REQUIRE(saw2);
}

TEST_CASE("MockAsyncFilesystem multiple requests all dispatched", "[async_fs]") {
    MockAsyncFilesystem fs;
    Collector col;
    fs.init();
    for (int i = 0; i < 3; ++i)
        fs.addFile("tile" + std::to_string(i) + ".bin", std::string(4, static_cast<char>('a' + i)));

    AsyncReadId ids[3];
    for (int i = 0; i < 3; ++i)
        ids[i] = fs.readFileAsync(PathDomain::Assets, ("tile" + std::to_string(i) + ".bin").c_str(), &col);

    fs.service();

    REQUIRE(col.results.size() == 3);
    for (auto& r : col.results)
        REQUIRE(r.status == AsyncReadStatus::Success);

    std::set<AsyncReadId> expected{ids[0], ids[1], ids[2]};
    for (auto& r : col.results)
        expected.erase(r.id);
    REQUIRE(expected.empty());
}

// #1083: a read is routed to the handler that issued it, so a null handler is REFUSED rather than
// accepted-and-dropped. A read whose completion goes nowhere is the silent failure per-request routing
// exists to remove; returning 0 tells the caller immediately instead of at never.
TEST_CASE("MockAsyncFilesystem refuses a read with no handler", "[async_fs]") {
    MockAsyncFilesystem fs;
    fs.init();
    fs.addFile("a.bin", "x");
    REQUIRE(fs.readFileAsync(PathDomain::Assets, "a.bin", nullptr) == 0);
    REQUIRE_NOTHROW(fs.service());
}

TEST_CASE("MockAsyncFilesystem readFileAsync returns 0 before init", "[async_fs]") {
    MockAsyncFilesystem fs;
    Collector col;
    fs.addFile("a.bin", "x");
    REQUIRE(fs.readFileAsync(PathDomain::Assets, "a.bin", &col) == 0);
}

TEST_CASE("MockAsyncFilesystem readFileAsync returns 0 for null path", "[async_fs]") {
    MockAsyncFilesystem fs;
    Collector col;
    fs.init();
    REQUIRE(fs.readFileAsync(PathDomain::Assets, nullptr, &col) == 0);
}
