// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "SDL3AsyncFilesystem.h"
#include "test_helpers.h"

using namespace fl;

#include <chrono>
#include <cstdint>
#include <fstream>
#include <set>
#include <string>
#include <thread>
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

// Writes content to path/name in a TempDir and returns the filename.
static void writeFile(const TempDir& dir, const std::string& name, const std::string& content) {
    std::ofstream f(dir.path / name, std::ios::binary);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// Calls fs.service() in a spin-loop until col.results.size() >= expected or timeout.
static void drainUntil(SDL3AsyncFilesystem& fs, Collector& col, std::size_t expected, int maxIterations = 200) {
    for (int i = 0; i < maxIterations && col.results.size() < expected; ++i) {
        fs.service();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

TEST_CASE("SDL3AsyncFilesystem reads existing file", "[async_fs_sdl3]") {
    TempDir dir;
    writeFile(dir, "tile0.bin", "hello world");

    SDL3AsyncFilesystem fs(dir.path, dir.path);
    Collector col;
    REQUIRE(fs.init());
    fs.setEventHandler(&col);

    AsyncReadId id = fs.readFileAsync(PathDomain::Assets, "tile0.bin");
    REQUIRE(id > 0);

    drainUntil(fs, col, 1);

    REQUIRE(col.results.size() == 1);
    REQUIRE(col.results[0].id == id);
    REQUIRE(col.results[0].status == AsyncReadStatus::Success);
    std::string got(col.results[0].data.begin(), col.results[0].data.end());
    REQUIRE(got == "hello world");

    fs.shutdown();
}

TEST_CASE("SDL3AsyncFilesystem fires Error for missing file", "[async_fs_sdl3]") {
    TempDir dir;
    SDL3AsyncFilesystem fs(dir.path, dir.path);
    Collector col;
    REQUIRE(fs.init());
    fs.setEventHandler(&col);

    AsyncReadId id = fs.readFileAsync(PathDomain::Assets, "no_such_file.bin");
    REQUIRE(id > 0);

    drainUntil(fs, col, 1);

    REQUIRE(col.results.size() == 1);
    REQUIRE(col.results[0].id == id);
    REQUIRE(col.results[0].status == AsyncReadStatus::Error);
    REQUIRE(!col.results[0].error.empty());

    fs.shutdown();
}

TEST_CASE("SDL3AsyncFilesystem multiple concurrent reads all complete", "[async_fs_sdl3]") {
    TempDir dir;
    const int N = 5;
    for (int i = 0; i < N; ++i)
        writeFile(dir, "tile" + std::to_string(i) + ".bin", std::string(64, static_cast<char>('a' + i)));

    SDL3AsyncFilesystem fs(dir.path, dir.path);
    Collector col;
    REQUIRE(fs.init());
    fs.setEventHandler(&col);

    AsyncReadId ids[N];
    for (int i = 0; i < N; ++i)
        ids[i] = fs.readFileAsync(PathDomain::Assets, ("tile" + std::to_string(i) + ".bin").c_str());

    drainUntil(fs, col, N);

    REQUIRE(col.results.size() == static_cast<std::size_t>(N));
    for (auto& r : col.results)
        REQUIRE(r.status == AsyncReadStatus::Success);

    std::set<AsyncReadId> expected(ids, ids + N);
    for (auto& r : col.results)
        expected.erase(r.id);
    REQUIRE(expected.empty());

    fs.shutdown();
}

TEST_CASE("SDL3AsyncFilesystem shutdown drains pending before returning", "[async_fs_sdl3]") {
    TempDir dir;
    writeFile(dir, "a.bin", "aaa");
    writeFile(dir, "b.bin", "bbb");

    SDL3AsyncFilesystem fs(dir.path, dir.path);
    Collector col;
    REQUIRE(fs.init());
    fs.setEventHandler(&col);

    AsyncReadId id1 = fs.readFileAsync(PathDomain::Assets, "a.bin");
    AsyncReadId id2 = fs.readFileAsync(PathDomain::Assets, "b.bin");

    // shutdown() must drain: all callbacks fire before it returns.
    fs.shutdown();

    REQUIRE(col.results.size() == 2);
    bool saw1 = false, saw2 = false;
    for (auto& r : col.results) {
        if (r.id == id1)
            saw1 = true;
        if (r.id == id2)
            saw2 = true;
    }
    REQUIRE(saw1);
    REQUIRE(saw2);
}

TEST_CASE("SDL3AsyncFilesystem reads empty file as Success with zero bytes", "[async_fs_sdl3]") {
    TempDir dir;
    writeFile(dir, "empty.bin", "");

    SDL3AsyncFilesystem fs(dir.path, dir.path);
    Collector col;
    REQUIRE(fs.init());
    fs.setEventHandler(&col);

    AsyncReadId id = fs.readFileAsync(PathDomain::Assets, "empty.bin");
    drainUntil(fs, col, 1);

    REQUIRE(col.results.size() == 1);
    REQUIRE(col.results[0].id == id);
    REQUIRE(col.results[0].status == AsyncReadStatus::Success);
    REQUIRE(col.results[0].data.empty());

    fs.shutdown();
}
