// SPDX-License-Identifier: GPL-3.0-or-later
#include "perf/PerformanceOverlay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

void PerformanceOverlay::cycleMode() {
    switch (m_mode) {
    case OverlayMode::Off:
        m_mode = OverlayMode::Compact;
        break;
    case OverlayMode::Compact:
        m_mode = OverlayMode::Full;
        break;
    case OverlayMode::Full:
        m_mode = OverlayMode::Off;
        break;
    }
}

void PerformanceOverlay::update(const FrameStats& stats, uint32_t entityCount, float simTickMs) {
    m_history[m_histHead] = stats.frameDtMs;
    m_histHead = (m_histHead + 1) % kHistoryLen;

    if (m_mode != OverlayMode::Off)
        buildLines(stats, entityCount, simTickMs);
}

std::span<const std::string_view> PerformanceOverlay::lines() const noexcept {
    if (m_mode == OverlayMode::Off)
        return {};
    return {m_lineViews, static_cast<std::size_t>(m_lineCount)};
}

void PerformanceOverlay::buildLines(const FrameStats& stats, uint32_t entityCount, float simTickMs) {
    m_lineCount = 0;
    char buf[128];

    float fps = stats.frameDtMs > 0.0f ? 1000.0f / stats.frameDtMs : 0.0f;

    if (m_mode == OverlayMode::Compact) {
        if (stats.gpuDtMs > 0.0f)
            std::snprintf(buf, sizeof(buf), "FPS: %.0f  Frame: %.1f ms  GPU: %.1f ms", fps, stats.frameDtMs,
                          stats.gpuDtMs);
        else
            std::snprintf(buf, sizeof(buf), "FPS: %.0f  Frame: %.1f ms", fps, stats.frameDtMs);
        m_line[0] = buf;
        m_lineViews[0] = m_line[0];
        m_lineCount = 1;
        return;
    }

    // Full mode: 4-5 lines.
    int line = 0;

    if (stats.gpuDtMs > 0.0f)
        std::snprintf(buf, sizeof(buf), "Frame: %.1f ms  GPU: %.1f ms  FPS: %.0f", stats.frameDtMs, stats.gpuDtMs, fps);
    else
        std::snprintf(buf, sizeof(buf), "Frame: %.1f ms  FPS: %.0f", stats.frameDtMs, fps);
    m_line[line] = buf;
    m_lineViews[line] = m_line[line];
    ++line;

    std::snprintf(buf, sizeof(buf), "Sim: %.1f ms  Entities: %u  Draw calls: %u", simTickMs, entityCount,
                  stats.drawCalls);
    m_line[line] = buf;
    m_lineViews[line] = m_line[line];
    ++line;

    if (stats.gpuMemBudgetBytes > 0) {
        uint64_t usedMb = stats.gpuMemUsedBytes / (1024 * 1024);
        uint64_t budgetMb = stats.gpuMemBudgetBytes / (1024 * 1024);
        std::snprintf(buf, sizeof(buf), "GPU mem: %llu MB / %llu MB", static_cast<unsigned long long>(usedMb),
                      static_cast<unsigned long long>(budgetMb));
        m_line[line] = buf;
        m_lineViews[line] = m_line[line];
        ++line;
    }

    // Bar graph: 128-sample rolling frame-time history.
    // Each position represents one sample, scaled to the max observed value.
    // U+0020 SPACE, U+2591 LIGHT SHADE, U+2592 MEDIUM SHADE,
    // U+2593 DARK SHADE, U+2588 FULL BLOCK (UTF-8 encoded).
    {
        float maxVal = 0.0f;
        for (int i = 0; i < kHistoryLen; ++i)
            maxVal = std::max(maxVal, m_history[i]);
        if (maxVal <= 0.0f)
            maxVal = 1.0f; // guard against divide-by-zero on first frame

        static constexpr const char* kBlocks[] = {
            " ",            // U+0020 SPACE
            "\xe2\x96\x91", // U+2591 LIGHT SHADE  ░
            "\xe2\x96\x92", // U+2592 MEDIUM SHADE ▒
            "\xe2\x96\x93", // U+2593 DARK SHADE   ▓
            "\xe2\x96\x88", // U+2588 FULL BLOCK   █
        };
        static constexpr int kNumBlocks = 5;

        m_line[line].clear();
        m_line[line].reserve(kHistoryLen * 3);
        for (int i = 0; i < kHistoryLen; ++i) {
            int histIdx = (m_histHead + i) % kHistoryLen;
            float norm = m_history[histIdx] / maxVal;
            int idx = static_cast<int>(norm * (kNumBlocks - 1) + 0.5f);
            idx = std::clamp(idx, 0, kNumBlocks - 1);
            m_line[line] += kBlocks[idx];
        }
        m_lineViews[line] = m_line[line];
        ++line;
    }

    m_lineCount = line;
}
