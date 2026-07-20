// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// RecordScheduler (#916) — the pure capture-boundary scheduler behind the cinematic recorder. It maps
// the 60 Hz snapshot tick stream to fixed-fps output-frame boundaries and counts duplicated frames,
// with NO rendering, HAL, or wall-clock dependency, so the honesty mechanism ("one frame per boundary,
// fail loud on drops") is fully unit-testable.
//
// The recorder emits one video frame per boundary tick (boundaries are `ticksPerFrame` apart, where
// ticksPerFrame = round(60 / fps)). Boundaries start at the first snapshot tick observed (the moment
// recording begins) and step forward. Each render-loop iteration reports the latest available snapshot
// tick; the scheduler returns how many boundary frames are now due. The caller renders ONCE and pushes
// that many frames — the first is a fresh render, any extra are duplicates of it (the sim advanced past
// more than one boundary between renders, i.e. the client fell behind), which the scheduler counts.

#include <cmath>
#include <cstdint>

namespace fl {

class RecordScheduler {
  public:
    explicit RecordScheduler(int recordFps) {
        const long tpf = std::lround(60.0 / (recordFps > 0 ? recordFps : 30));
        m_ticksPerFrame = static_cast<uint64_t>(tpf < 1 ? 1 : tpf);
    }

    [[nodiscard]] uint64_t ticksPerFrame() const noexcept {
        return m_ticksPerFrame;
    }

    // The number of boundary frames due at `latestTick` that have not been emitted yet, advancing
    // internal state. On the first call it anchors the boundary grid at `latestTick` and returns 1
    // (the recording's first frame). Subsequent calls return >= 0.
    //
    // Contract: the caller renders ONE fresh frame per call that returns n >= 1, then pushes n frames to
    // the encoder (the first fresh, the remaining n-1 duplicates of it — the sim advanced past multiple
    // boundaries between renders, i.e. the client fell behind). So this call counts n-1 duplicates.
    int boundariesDue(uint64_t latestTick) noexcept {
        if (!m_started) {
            m_started = true;
            m_nextBoundary = latestTick;
        }
        int n = 0;
        while (latestTick >= m_nextBoundary) {
            m_nextBoundary += m_ticksPerFrame;
            ++n;
        }
        if (n > 0) {
            m_totalFrames += static_cast<uint64_t>(n);
            m_dupFrames += static_cast<uint64_t>(n - 1); // one fresh render this call; the rest are dups
        }
        return n;
    }

    [[nodiscard]] uint64_t totalFrames() const noexcept {
        return m_totalFrames;
    }
    [[nodiscard]] uint64_t dupFrames() const noexcept {
        return m_dupFrames;
    }
    [[nodiscard]] uint64_t nextBoundaryTick() const noexcept {
        return m_nextBoundary;
    }

  private:
    uint64_t m_ticksPerFrame{2};
    uint64_t m_nextBoundary{0};
    uint64_t m_totalFrames{0};
    uint64_t m_dupFrames{0};
    bool m_started{false};
};

} // namespace fl
