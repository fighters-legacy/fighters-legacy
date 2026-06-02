// SPDX-License-Identifier: GPL-3.0-or-later
#include "perf/PerformanceOverlay.h"

#include "Utf8Decode.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <unordered_set>

static size_t countCodepoints(std::string_view s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        if (b < 0x80)
            i += 1;
        else if ((b & 0xE0u) == 0xC0u)
            i += 2;
        else if ((b & 0xF0u) == 0xE0u)
            i += 3;
        else
            i += 4;
        ++n;
    }
    return n;
}

TEST_CASE("PerformanceOverlay: F3 cycle Off->Compact->Full->Off", "[perf_overlay]") {
    PerformanceOverlay ov;
    CHECK(ov.mode() == OverlayMode::Off);

    ov.cycleMode();
    CHECK(ov.mode() == OverlayMode::Compact);

    ov.cycleMode();
    CHECK(ov.mode() == OverlayMode::Full);

    ov.cycleMode();
    CHECK(ov.mode() == OverlayMode::Off);
}

TEST_CASE("PerformanceOverlay: lines() returns empty span when Off", "[perf_overlay]") {
    PerformanceOverlay ov;
    ov.setMode(OverlayMode::Off);

    FrameStats stats{};
    stats.frameDtMs = 16.6f;
    ov.update(stats, 5, 1000.0f / 60.0f);

    CHECK(ov.lines().empty());
}

TEST_CASE("PerformanceOverlay: Compact mode produces exactly one line with FPS", "[perf_overlay]") {
    PerformanceOverlay ov;
    ov.setMode(OverlayMode::Compact);

    FrameStats stats{};
    stats.frameDtMs = 8.3f;
    stats.gpuDtMs = 6.1f;
    ov.update(stats, 10, 16.7f);

    auto lines = ov.lines();
    REQUIRE(lines.size() == 1u);
    CHECK(std::string(lines[0]).find("FPS") != std::string::npos);
    CHECK(std::string(lines[0]).find("Frame") != std::string::npos);
}

TEST_CASE("PerformanceOverlay: Full mode bar graph line has exactly 128 codepoints", "[perf_overlay]") {
    PerformanceOverlay ov;
    ov.setMode(OverlayMode::Full);

    FrameStats stats{};
    stats.frameDtMs = 16.6f;
    stats.gpuDtMs = 14.0f;
    stats.gpuMemUsedBytes = 256 * 1024 * 1024;
    stats.gpuMemBudgetBytes = 2048 * 1024 * 1024ULL;
    for (int i = 0; i < 10; ++i)
        ov.update(stats, 5, 16.7f);

    auto lines = ov.lines();
    REQUIRE(lines.size() >= 3u);
    // Last line is the bar graph: exactly 128 Unicode codepoints.
    CHECK(countCodepoints(lines.back()) == 128u);
}

TEST_CASE("PerformanceOverlay: zero-history guard: no crash on all-zero stats", "[perf_overlay]") {
    PerformanceOverlay ov;
    ov.setMode(OverlayMode::Full);

    FrameStats zeroStats{};
    // First update with all zeros; should not divide by zero.
    REQUIRE_NOTHROW(ov.update(zeroStats, 0, 0.0f));
    auto lines = ov.lines();
    REQUIRE(lines.size() >= 1u);
    // Bar graph must be 128 codepoints even with zero history.
    CHECK(countCodepoints(lines.back()) == 128u);
}

TEST_CASE("PerformanceOverlay: history ring buffer wraps at 128 samples", "[perf_overlay]") {
    PerformanceOverlay ov;
    ov.setMode(OverlayMode::Full);

    FrameStats stats{};
    // Fill 200 samples — more than twice the ring size.
    for (int i = 0; i < 200; ++i) {
        stats.frameDtMs = static_cast<float>(i % 30);
        ov.update(stats, 0, 16.7f);
    }
    // Should not crash, and bar graph is still 128 codepoints.
    auto lines = ov.lines();
    REQUIRE(!lines.empty());
    CHECK(countCodepoints(lines.back()) == 128u);
}

TEST_CASE("PerformanceOverlay: Full mode bar graph uses only the five expected Unicode block chars", "[perf_overlay]") {
    PerformanceOverlay ov;
    ov.setMode(OverlayMode::Full);

    FrameStats stats{};
    stats.frameDtMs = 16.6f;
    for (int i = 0; i < 10; ++i)
        ov.update(stats, 0, 16.7f);

    auto lines = ov.lines();
    REQUIRE(!lines.empty());

    // Allowed codepoints: SPACE, LIGHT SHADE, MEDIUM SHADE, DARK SHADE, FULL BLOCK.
    static const std::unordered_set<uint32_t> kAllowed = {0x0020u, 0x2591u, 0x2592u, 0x2593u, 0x2588u};
    const std::string_view bar = lines.back();
    const char* p = bar.data();
    const char* end = p + bar.size();
    while (p < end) {
        const uint32_t cp = fl::nextUtf8Codepoint(p, end);
        CHECK(kAllowed.count(cp) == 1u);
    }
}

TEST_CASE("PerformanceOverlay: setMode persists across update calls", "[perf_overlay]") {
    PerformanceOverlay ov;
    ov.setMode(OverlayMode::Full);
    CHECK(ov.mode() == OverlayMode::Full);

    FrameStats stats{};
    ov.update(stats, 0, 16.7f);
    CHECK(ov.mode() == OverlayMode::Full);
}
