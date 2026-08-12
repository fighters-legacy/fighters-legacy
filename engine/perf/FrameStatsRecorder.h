// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// FrameStatsRecorder — the client-side render-perf artifact, the sibling of fl-server's
// --metrics-json (engine/perf/ServerTickReport.h) and deliberately shaped like it.
//
// The game records one FrameSample per rendered FLIGHT frame and serialises the run as a single
// JSON document: a summary (fl::Stats over frame time / GPU time / VRAM) plus the RAW per-frame
// samples. The raw samples are not optional and not a debug extra — they carry a wall-clock epoch
// timestamp, which is what lets an external tool correlate frames against something else happening
// on the same machine. That is the entire mechanism behind the #782 GPU-contention harness
// (tools/gpu_contention/): the burst driver records when it drove the LLM in epoch-ms, the recorder
// records when each frame happened in epoch-ms, and the analyzer joins the two. A summary-only
// report cannot answer "what did the frame time do WHILE the model was running".
//
// Both sides use wall-clock (std::chrono::system_clock / Python time.time()) rather than a
// monotonic clock precisely because they are different processes: a monotonic clock's epoch is
// per-process and unjoinable. Burst windows are tens of seconds, so ms-scale agreement is orders of
// magnitude more precision than the join needs.
//
// Header-only and dependency-free (engine/perf/Stats.h + platform/RenderTypes.h + stdlib), so the
// game, tests, and any future tool can include it. The atomic write stays at the CALL site (via
// fl::writeConfigFile) — same split as ServerTickReport, which keeps this header free of any
// filesystem or logging dependency.
//
// fromJson() parses the SUMMARY ONLY, in the same tolerant strtod style as ServerTickReport
// (NOT std::from_chars, which lacks floating-point support on Apple Clang). It deliberately does
// not parse the sample array: raw-sample analysis lives in Python, and a hand-rolled JSON array
// parser in C++ would be a second implementation with no consumer.

#include "JsonScan.h"
#include "Stats.h"
#include "util/Json.h"

#include "RenderTypes.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

// Versioning discipline is ServerTickReport's (#686): the format is ADDITIVE and NAME-KEYED, every
// consumer looks fields up by name and ignores what it does not recognise, and both producer and
// consumer land in the same commit. Bump only if a field's MEANING changes under an unchanged name,
// or one is removed.
inline constexpr int kFrameStatsSchemaVersion = 1;

// Hitch thresholds. 33.3 ms is "dropped below 30 fps for this frame"; 50 ms is the stall band a
// player feels as a stutter rather than as slowness.
inline constexpr double kHitchThresholdMs = 33.3;
inline constexpr double kStallThresholdMs = 50.0;

// One rendered frame. tMs is wall-clock epoch milliseconds — see the header comment: it is the
// join key, not a convenience.
struct FrameSample {
    double tMs{0.0};
    float frameDtMs{0.0f};
    float gpuDtMs{0.0f};
    double gpuMemUsedMb{0.0};
};

// The parsed summary of a report (what fromJson fills). The raw samples are not represented here.
struct FrameStatsSummary {
    int schemaVersion{kFrameStatsSchemaVersion};
    double startedEpochMs{0.0};
    double durationSeconds{0.0};
    uint64_t frames{0};
    Stats frameMs{};
    Stats gpuMs{};
    Stats gpuMemUsedMb{};
    double gpuMemBudgetMb{0.0};
    uint64_t hitches{0}; // frames slower than kHitchThresholdMs
    uint64_t stalls{0};  // frames slower than kStallThresholdMs
};

class FrameStatsRecorder {
  public:
    // Renderer/scene provenance, written into the report header so a results file identifies the
    // machine and workload it came from without a side-channel.
    void setGpuInfo(std::string info) {
        m_gpuInfo = std::move(info);
    }
    void setScene(std::string scene) {
        m_scene = std::move(scene);
    }

    void record(const FrameStats& s, double epochMs) {
        if (m_samples.empty()) {
            m_samples.reserve(kReserveSamples);
            m_startedEpochMs = epochMs;
            m_lastFlushMs = epochMs;
        }
        FrameSample fs;
        fs.tMs = epochMs;
        fs.frameDtMs = s.frameDtMs;
        fs.gpuDtMs = s.gpuDtMs;
        fs.gpuMemUsedMb = bytesToMb(s.gpuMemUsedBytes);
        m_samples.push_back(fs);
        // The budget is a device property, near-constant across a run — carried once in the header
        // rather than repeated on every sample.
        m_gpuMemBudgetMb = bytesToMb(s.gpuMemBudgetBytes);
        m_lastEpochMs = epochMs;
    }

    // Periodic-flush gate. The flush exists so a run survives the orchestrator killing the process
    // (SIGTERM / Stop-Process never reach the clean-exit write) — on a measurement run that is the
    // difference between a result and a wasted GPU hour.
    bool shouldFlush(double epochMs, double intervalMs = kFlushIntervalMs) const {
        return !m_samples.empty() && (epochMs - m_lastFlushMs) >= intervalMs;
    }
    void markFlushed(double epochMs) {
        m_lastFlushMs = epochMs;
    }

    std::size_t sampleCount() const {
        return m_samples.size();
    }
    const std::vector<FrameSample>& samples() const {
        return m_samples;
    }

    std::string toJson() const;

  private:
    static constexpr std::size_t kReserveSamples = 65536; // ~18 min at 60 fps before a realloc
    static constexpr double kFlushIntervalMs = 5000.0;

    static double bytesToMb(uint64_t bytes) {
        return static_cast<double>(bytes) / (1024.0 * 1024.0);
    }

    std::vector<FrameSample> m_samples;
    std::string m_gpuInfo;
    std::string m_scene;
    double m_gpuMemBudgetMb{0.0};
    double m_startedEpochMs{0.0};
    double m_lastEpochMs{0.0};
    double m_lastFlushMs{0.0};
};

inline std::string FrameStatsRecorder::toJson() const {
    // computeStats sorts in place, so build throwaway copies of each series.
    std::vector<double> frameMs, gpuMs, memMb;
    frameMs.reserve(m_samples.size());
    gpuMs.reserve(m_samples.size());
    memMb.reserve(m_samples.size());
    uint64_t hitches = 0, stalls = 0;
    for (const auto& s : m_samples) {
        frameMs.push_back(static_cast<double>(s.frameDtMs));
        gpuMs.push_back(static_cast<double>(s.gpuDtMs));
        memMb.push_back(s.gpuMemUsedMb);
        if (s.frameDtMs > kHitchThresholdMs)
            ++hitches;
        if (s.frameDtMs > kStallThresholdMs)
            ++stalls;
    }
    const Stats fs = computeStats(frameMs);
    const Stats gs = computeStats(gpuMs);
    const Stats ms = computeStats(memMb);
    const double durationS = m_samples.empty() ? 0.0 : (m_lastEpochMs - m_startedEpochMs) / 1000.0;

    char head[1536];
    std::snprintf(head, sizeof(head),
                  "{\n"
                  "  \"schema_version\": %d,\n"
                  "  \"started_epoch_ms\": %.1f,\n"
                  "  \"duration_s\": %.3f,\n"
                  "  \"frames\": %llu,\n"
                  "  \"gpu_info\": \"%s\",\n"
                  "  \"scene\": \"%s\",\n"
                  "  \"gpu_mem_budget_mb\": %.2f,\n"
                  "  \"hitches_over_33ms\": %llu,\n"
                  "  \"stalls_over_50ms\": %llu,\n",
                  kFrameStatsSchemaVersion, m_startedEpochMs, durationS,
                  static_cast<unsigned long long>(m_samples.size()), json::escape(m_gpuInfo).c_str(),
                  json::escape(m_scene).c_str(), m_gpuMemBudgetMb, static_cast<unsigned long long>(hitches),
                  static_cast<unsigned long long>(stalls));
    std::string out = head;

    auto statBlock = [](const char* name, const Stats& s) {
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "  \"%s\": { \"min\": %.4f, \"mean\": %.4f, \"max\": %.4f, \"p95\": %.4f, \"p99\": %.4f, "
                      "\"stddev\": %.4f },\n",
                      name, s.min, s.mean, s.max, s.p95, s.p99, s.stddev);
        return std::string(buf);
    };
    out += statBlock("frame_ms", fs);
    out += statBlock("gpu_ms", gs);
    out += statBlock("gpu_mem_used_mb", ms);

    // Array-of-arrays rather than per-sample objects: at 60 fps a five-minute run is 18k samples,
    // and the column names would otherwise be repeated 18k times for no reader benefit. The column
    // order is declared in the document so the consumer never hard-codes it.
    out += "  \"samples_columns\": [\"t_ms\", \"frame_ms\", \"gpu_ms\", \"gpu_mem_used_mb\"],\n";
    out += "  \"samples\": [";
    char row[128];
    for (std::size_t i = 0; i < m_samples.size(); ++i) {
        const auto& s = m_samples[i];
        std::snprintf(row, sizeof(row), "%s\n    [%.1f, %.3f, %.3f, %.2f]", i == 0 ? "" : ",", s.tMs,
                      static_cast<double>(s.frameDtMs), static_cast<double>(s.gpuDtMs), s.gpuMemUsedMb);
        out += row;
    }
    out += m_samples.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

// Parses the summary from a report (tolerant; ignores the sample array). Returns false if no
// recognisable field was found.
//
// Named parseFrameStats rather than fromJson: overloading a free `fromJson` across report types
// invites the wrong overload being selected through an implicit conversion, and these two documents
// are not interchangeable.
inline bool parseFrameStats(std::string_view json, FrameStatsSummary& out) {
    bool any = false;
    auto num = [&](std::string_view key, auto&& assign) {
        if (auto v = json::numberField(json, key)) {
            assign(*v);
            any = true;
        }
    };
    num("schema_version", [&](double v) { out.schemaVersion = static_cast<int>(v); });
    num("started_epoch_ms", [&](double v) { out.startedEpochMs = v; });
    num("duration_s", [&](double v) { out.durationSeconds = v; });
    num("frames", [&](double v) { out.frames = static_cast<uint64_t>(v); });
    num("gpu_mem_budget_mb", [&](double v) { out.gpuMemBudgetMb = v; });
    num("hitches_over_33ms", [&](double v) { out.hitches = static_cast<uint64_t>(v); });
    num("stalls_over_50ms", [&](double v) { out.stalls = static_cast<uint64_t>(v); });
    any |= detail::parseStat(json, "frame_ms", out.frameMs);
    any |= detail::parseStat(json, "gpu_ms", out.gpuMs);
    any |= detail::parseStat(json, "gpu_mem_used_mb", out.gpuMemUsedMb);
    return any;
}

} // namespace fl
