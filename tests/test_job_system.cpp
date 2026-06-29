// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "job/JobSystem.h"

#include <atomic>
#include <cstddef>
#include <numeric>
#include <stdexcept>
#include <vector>

using fl::JobSystem;
using fl::resolveWorkerCount;

TEST_CASE("resolveWorkerCount maps requested totals to background worker counts", "[job]") {
    // Explicit serial.
    CHECK(resolveWorkerCount(1, 8) == 0u);
    // N total -> N-1 background, independent of detected.
    CHECK(resolveWorkerCount(2, 8) == 1u);
    CHECK(resolveWorkerCount(8, 8) == 7u);
    CHECK(resolveWorkerCount(4, 1) == 3u);
    // Auto uses detected-1.
    CHECK(resolveWorkerCount(0, 8) == 7u);
    CHECK(resolveWorkerCount(0, 2) == 1u);
    // Auto on a single core degenerates to inline.
    CHECK(resolveWorkerCount(0, 1) == 0u);
    // Auto with undetectable hardware concurrency falls back to 4 total -> 3 background.
    CHECK(resolveWorkerCount(0, 0) == 3u);
}

TEST_CASE("JobSystem(1) runs inline with no background workers", "[job]") {
    JobSystem js(1);
    CHECK(js.workerCount() == 0u);

    std::vector<int> v(1000, 0);
    js.parallel_for(v.size(), 64, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i)
            v[i] = static_cast<int>(i) + 1;
    });
    for (std::size_t i = 0; i < v.size(); ++i)
        REQUIRE(v[i] == static_cast<int>(i) + 1);
}

TEST_CASE("parallel_for visits every index exactly once across worker counts", "[job]") {
    for (unsigned total : {1u, 2u, 4u, 8u}) {
        JobSystem js(total);
        const std::size_t n = 10000;
        std::vector<std::atomic<int>> hits(n);
        for (auto& h : hits)
            h.store(0);

        js.parallel_for(n, 33, [&](std::size_t b, std::size_t e) {
            for (std::size_t i = b; i < e; ++i)
                hits[i].fetch_add(1, std::memory_order_relaxed);
        });

        for (std::size_t i = 0; i < n; ++i)
            REQUIRE(hits[i].load() == 1);
    }
}

TEST_CASE("parallel_for produces the correct sum (parallel reduction via atomics)", "[job]") {
    JobSystem js(4);
    std::vector<std::size_t> data(5000);
    std::iota(data.begin(), data.end(), std::size_t{1});
    const std::size_t expected = std::accumulate(data.begin(), data.end(), std::size_t{0});

    std::atomic<std::size_t> sum{0};
    js.parallel_for(data.size(), 50, [&](std::size_t b, std::size_t e) {
        std::size_t local = 0;
        for (std::size_t i = b; i < e; ++i)
            local += data[i];
        sum.fetch_add(local, std::memory_order_relaxed);
    });
    REQUIRE(sum.load() == expected);
}

TEST_CASE("parallel_for handles grain edge cases", "[job]") {
    JobSystem js(4);

    SECTION("count zero is a no-op and never invokes fn") {
        std::atomic<int> calls{0};
        js.parallel_for(0, 16, [&](std::size_t, std::size_t) { calls.fetch_add(1); });
        CHECK(calls.load() == 0);
    }

    SECTION("count smaller than grain runs as a single chunk") {
        std::vector<int> v(5, 0);
        js.parallel_for(v.size(), 64, [&](std::size_t b, std::size_t e) {
            for (std::size_t i = b; i < e; ++i)
                v[i] = 7;
        });
        for (int x : v)
            CHECK(x == 7);
    }

    SECTION("grain zero is clamped to one and still covers the range") {
        std::vector<int> v(100, 0);
        js.parallel_for(v.size(), 0, [&](std::size_t b, std::size_t e) {
            for (std::size_t i = b; i < e; ++i)
                v[i] = 1;
        });
        for (int x : v)
            CHECK(x == 1);
    }

    SECTION("non-divisible count covers the remainder chunk") {
        const std::size_t n = 1003; // not a multiple of grain
        std::vector<std::atomic<int>> hits(n);
        for (auto& h : hits)
            h.store(0);
        js.parallel_for(n, 100, [&](std::size_t b, std::size_t e) {
            for (std::size_t i = b; i < e; ++i)
                hits[i].fetch_add(1);
        });
        for (std::size_t i = 0; i < n; ++i)
            CHECK(hits[i].load() == 1);
    }
}

TEST_CASE("parallel_for rethrows a chunk exception and the pool survives", "[job]") {
    JobSystem js(4);

    REQUIRE_THROWS_AS(js.parallel_for(1000, 50,
                                      [&](std::size_t b, std::size_t /*e*/) {
                                          if (b == 0)
                                              throw std::runtime_error("boom");
                                      }),
                      std::runtime_error);

    // The pool is reusable after an exception.
    std::vector<int> v(500, 0);
    js.parallel_for(v.size(), 32, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i)
            v[i] = 3;
    });
    for (int x : v)
        REQUIRE(x == 3);
}

TEST_CASE("JobSystem survives many small back-to-back dispatches", "[job]") {
    // Stress the latch/condition-variable wakeup path; the meaningful target under TSan/ASan.
    JobSystem js(8);
    std::atomic<std::size_t> total{0};
    for (int iter = 0; iter < 2000; ++iter) {
        js.parallel_for(64, 8,
                        [&](std::size_t b, std::size_t e) { total.fetch_add(e - b, std::memory_order_relaxed); });
    }
    REQUIRE(total.load() == static_cast<std::size_t>(2000) * 64u);
}
