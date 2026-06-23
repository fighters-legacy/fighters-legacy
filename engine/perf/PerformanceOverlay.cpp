// SPDX-License-Identifier: GPL-3.0-or-later
#include "perf/PerformanceOverlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <glm/trigonometric.hpp>

namespace fl {

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

void PerformanceOverlay::setSceneInfo(const char* modeStr, const CameraView& cam, const glm::dvec3* entityPos,
                                      double terrainAtCam, double terrainAtEntity) {
    if (m_mode == OverlayMode::Off)
        return;

    char buf[160];
    int line = m_lineCount;

    // Camera eye is the worldOrigin; the look direction is -Z of the view rotation
    // (glm::lookAt stores the negated forward in the third column).
    const glm::dvec3 eye = cam.worldOrigin;
    const glm::vec3 fwd = -glm::vec3(cam.view[0][2], cam.view[1][2], cam.view[2][2]);
    const float fwdPitch = glm::degrees(std::asin(std::clamp(fwd.y, -1.0f, 1.0f)));

    if (line < kMaxLines) {
        std::snprintf(buf, sizeof(buf), "CAM %s eye=(%.1f, %.1f, %.1f) pitch=%+.1f AGL=%.1f", modeStr, eye.x, eye.y,
                      eye.z, fwdPitch, eye.y - terrainAtCam);
        m_line[line] = buf;
        m_lineViews[line] = m_line[line];
        ++line;
    }

    if (entityPos && line < kMaxLines) {
        // Pitch from the camera eye to the entity, so it is obvious when the camera is aimed
        // above the entity (positive gap = entity is below the look direction).
        const glm::dvec3 toEnt = *entityPos - eye;
        const double dist = glm::length(toEnt);
        const float entPitch =
            dist > 1e-6 ? glm::degrees(std::asin(std::clamp(static_cast<float>(toEnt.y / dist), -1.0f, 1.0f))) : 0.0f;
        std::snprintf(buf, sizeof(buf), "ENT pos=(%.1f, %.1f, %.1f) AGL=%.1f  to-ent pitch=%+.1f dist=%.0f",
                      entityPos->x, entityPos->y, entityPos->z, entityPos->y - terrainAtEntity, entPitch, dist);
        m_line[line] = buf;
        m_lineViews[line] = m_line[line];
        ++line;
    }

    m_lineCount = line;
}

void PerformanceOverlay::setPing(bool show, bool hasRtt, uint32_t pingMs) noexcept {
    m_showPing = show;
    if (show) {
        m_pingLineStr = hasRtt ? ("Ping: " + std::to_string(pingMs) + " ms") : "Ping: -- ms";
        m_pingLineView = m_pingLineStr;
    }
}

std::span<const std::string_view> PerformanceOverlay::lines() const noexcept {
    if (m_showPing && m_lineCount < kMaxLines) {
        m_lineViews[m_lineCount] = m_pingLineView;
        if (m_mode == OverlayMode::Off)
            return {m_lineViews, 1u}; // ping-only when F3 is off
        return {m_lineViews, static_cast<std::size_t>(m_lineCount + 1)};
    }
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

} // namespace fl
