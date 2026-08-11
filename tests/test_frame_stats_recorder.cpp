// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for engine/perf/FrameStatsRecorder.h — the client-side render-perf artifact behind the
// #782 GPU-contention harness.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "perf/FrameStatsRecorder.h"

#include <string>

using Catch::Approx;

namespace {

fl::FrameStats mkStats(float frameMs, float gpuMs = 4.0f, double usedMb = 2048.0, double budgetMb = 8192.0) {
    fl::FrameStats s{};
    s.frameDtMs = frameMs;
    s.gpuDtMs = gpuMs;
    s.gpuMemUsedBytes = static_cast<uint64_t>(usedMb * 1024.0 * 1024.0);
    s.gpuMemBudgetBytes = static_cast<uint64_t>(budgetMb * 1024.0 * 1024.0);
    return s;
}

} // namespace

TEST_CASE("empty recorder emits a valid zeroed document", "[framestats]") {
    fl::FrameStatsRecorder r;
    REQUIRE(r.sampleCount() == 0);

    const std::string json = r.toJson();
    fl::FrameStatsSummary sum;
    REQUIRE(fl::parseFrameStats(json, sum));
    CHECK(sum.frames == 0);
    CHECK(sum.durationSeconds == Approx(0.0));
    CHECK(sum.frameMs.max == Approx(0.0));
    // An empty sample list must still close the array, or the document is unparseable downstream.
    CHECK(json.find("\"samples\": []") != std::string::npos);
}

TEST_CASE("summary round-trips through parseFrameStats", "[framestats]") {
    fl::FrameStatsRecorder r;
    r.setGpuInfo("Test GPU");
    r.setScene("builtin:sandbox");
    // 100 frames at a steady 10 ms, timestamps 10 ms apart.
    for (int i = 0; i < 100; ++i)
        r.record(mkStats(10.0f, 6.0f, 3000.0, 12000.0), 1'700'000'000'000.0 + i * 10.0);

    fl::FrameStatsSummary sum;
    REQUIRE(fl::parseFrameStats(r.toJson(), sum));
    CHECK(sum.schemaVersion == fl::kFrameStatsSchemaVersion);
    CHECK(sum.frames == 100);
    CHECK(sum.startedEpochMs == Approx(1'700'000'000'000.0));
    CHECK(sum.durationSeconds == Approx(0.99)); // last minus first, not count * dt
    CHECK(sum.frameMs.mean == Approx(10.0));
    CHECK(sum.gpuMs.mean == Approx(6.0));
    CHECK(sum.gpuMemUsedMb.mean == Approx(3000.0).margin(0.01));
    CHECK(sum.gpuMemBudgetMb == Approx(12000.0).margin(0.01));
    CHECK(sum.hitches == 0);
    CHECK(sum.stalls == 0);
}

TEST_CASE("hitches and stalls are counted at their own thresholds", "[framestats]") {
    fl::FrameStatsRecorder r;
    double t = 0.0;
    auto add = [&](float ms) { r.record(mkStats(ms), t += 16.0); };
    for (int i = 0; i < 10; ++i)
        add(16.0f); // healthy
    add(34.0f);     // hitch only
    add(40.0f);     // hitch only
    add(120.0f);    // hitch AND stall
    // Exactly at the hitch threshold — the comparison is strictly-greater, so it must NOT count.
    add(static_cast<float>(fl::kHitchThresholdMs));

    fl::FrameStatsSummary sum;
    REQUIRE(fl::parseFrameStats(r.toJson(), sum));
    CHECK(sum.hitches == 3);
    CHECK(sum.stalls == 1);
}

TEST_CASE("a frame slower than the old 50 ms particle cap survives to the report", "[framestats]") {
    // The renderer caps the PARTICLE timestep at 50 ms but reports frame time uncapped, because a
    // contention run exists to measure exactly the frames worse than that (a model load stalls the
    // GPU for hundreds of ms). If frameDtMs is ever re-capped at the source, this is the regression.
    fl::FrameStatsRecorder r;
    r.record(mkStats(8.0f), 1000.0);
    r.record(mkStats(450.0f), 1450.0);

    fl::FrameStatsSummary sum;
    REQUIRE(fl::parseFrameStats(r.toJson(), sum));
    CHECK(sum.frameMs.max == Approx(450.0));
    CHECK(sum.stalls == 1);
}

TEST_CASE("raw samples carry the join key and are emitted in order", "[framestats]") {
    fl::FrameStatsRecorder r;
    r.record(mkStats(11.0f), 5000.0);
    r.record(mkStats(12.0f), 5016.0);
    r.record(mkStats(13.0f), 5033.0);
    REQUIRE(r.sampleCount() == 3);
    CHECK(r.samples()[0].tMs == Approx(5000.0));
    CHECK(r.samples()[2].frameDtMs == Approx(13.0));

    const std::string json = r.toJson();
    // The column order must be declared in the document — the analyzer reads it rather than
    // hard-coding positions.
    REQUIRE(json.find("\"samples_columns\": [\"t_ms\", \"frame_ms\", \"gpu_ms\", \"gpu_mem_used_mb\"]") !=
            std::string::npos);
    const auto first = json.find("[5000.0,");
    const auto last = json.find("[5033.0,");
    REQUIRE(first != std::string::npos);
    REQUIRE(last != std::string::npos);
    CHECK(first < last);
}

TEST_CASE("provenance strings are escaped", "[framestats]") {
    fl::FrameStatsRecorder r;
    // Real device strings carry quotes and (on Windows) backslashes; an unescaped one would
    // silently corrupt the document.
    r.setGpuInfo("NVIDIA \"RTX 5080\" C:\\driver");
    r.setScene("builtin:sandbox");
    r.record(mkStats(10.0f), 1.0);

    const std::string json = r.toJson();
    CHECK(json.find("\\\"RTX 5080\\\"") != std::string::npos);
    CHECK(json.find("C:\\\\driver") != std::string::npos);
    // And it must still parse.
    fl::FrameStatsSummary sum;
    CHECK(fl::parseFrameStats(json, sum));
    CHECK(fl::json::stringField(json, "gpu_info").value_or("") == "NVIDIA \"RTX 5080\" C:\\driver");
    CHECK(fl::json::stringField(json, "scene").value_or("") == "builtin:sandbox");
}

TEST_CASE("parseFrameStats is tolerant of garbage and partial documents", "[framestats]") {
    fl::FrameStatsSummary sum;
    CHECK_FALSE(fl::parseFrameStats("", sum));
    CHECK_FALSE(fl::parseFrameStats("not json at all", sum));
    CHECK_FALSE(fl::parseFrameStats("{ \"unrelated\": 3 }", sum));

    // A partial document yields what it has and ignores the rest — the additive/name-keyed contract.
    fl::FrameStatsSummary partial;
    REQUIRE(fl::parseFrameStats("{ \"frames\": 42, \"future_field\": 7 }", partial));
    CHECK(partial.frames == 42);
    CHECK(partial.frameMs.p99 == Approx(0.0));
}

TEST_CASE("shouldFlush gates on the interval and only once samples exist", "[framestats]") {
    fl::FrameStatsRecorder r;
    // No samples: nothing to flush, however much time passes.
    CHECK_FALSE(r.shouldFlush(1'000'000.0));

    r.record(mkStats(10.0f), 1000.0);
    CHECK_FALSE(r.shouldFlush(2000.0, 5000.0));
    CHECK(r.shouldFlush(6000.0, 5000.0));
    r.markFlushed(6000.0);
    CHECK_FALSE(r.shouldFlush(7000.0, 5000.0));
    CHECK(r.shouldFlush(11'000.0, 5000.0));
}
