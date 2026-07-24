// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "voice/VoiceCodec.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace fl {

// ---------------------------------------------------------------------------------------------
// Per-speaker voice jitter buffer (#531)
// ---------------------------------------------------------------------------------------------
// The input jitter buffer (engine/net/JitterBuffer.h) is a DEPTH-ONLY ring: it holds control
// samples and stale-repeats the last one on underrun, because a repeated stick position is a
// perfectly good guess. Voice cannot borrow it. Audio needs three things that buffer does not do:
//
//   1. REORDERING by sequence number. A control packet that arrives late is worthless and gets
//      discarded; a voice frame that arrives late is a syllable, and playing it out of order is
//      audibly wrong in a way a stale stick position never is.
//   2. An explicit LOSS signal. Underrun must reach the decoder as "conceal this frame", not as
//      "repeat the last one" — repeating an Opus frame produces a robotic stutter, whereas Opus's
//      own PLC extrapolates the glottal pulse and is nearly inaudible for one or two frames.
//   3. PREFILL. Playback must not start until `targetDepth` frames are held, or the first jitter
//      spike underruns immediately and the transmission opens with a stutter.
//
// So this is its own type. Pure, allocation-bounded, no audio dependency — unit-testable without a
// codec or a device. Sequence comparison is a half-window wrap compare, so a 16-bit counter
// wrapping mid-call is not a special case (that is ~22 minutes of continuous speech at 20 ms).
// ---------------------------------------------------------------------------------------------

class VoiceJitterBuffer {
  public:
    enum class Pop : uint8_t {
        Ok,      // a frame was produced
        Conceal, // the frame at the playhead never arrived: run PLC and advance
        Empty,   // nothing buffered / still prefilling: play silence and do NOT advance the playhead
    };

    // targetDepth: frames held before playback starts (and the low-water mark the buffer defends).
    // maxDepth: hard cap; a burst beyond this drops the OLDEST frames, because in a live call the
    // newest audio is the one worth keeping.
    explicit VoiceJitterBuffer(uint32_t targetDepth = kDefaultTargetDepth,
                               uint32_t maxDepth = kDefaultMaxDepth) noexcept {
        setDepth(targetDepth, maxDepth);
    }

    void setDepth(uint32_t targetDepth, uint32_t maxDepth) noexcept {
        m_maxDepth = maxDepth < 1u ? 1u : (maxDepth > kHardMaxDepth ? kHardMaxDepth : maxDepth);
        m_targetDepth = targetDepth < 1u ? 1u : (targetDepth > m_maxDepth ? m_maxDepth : targetDepth);
    }

    // Insert a received frame. Duplicates and frames already behind the playhead are dropped.
    void push(uint16_t seq, std::span<const uint8_t> payload) {
        if (payload.size() > kMaxVoicePayloadBytes)
            return; // over-long payloads never enter the pipeline (see VoiceCodec.h)
        if (m_started && !isNewerOrEqual(seq, m_playhead))
            return; // already played past it: a late frame is a lost frame
        for (const auto& f : m_frames) {
            if (f.seq == seq)
                return; // duplicate
        }
        VoiceFrame f;
        f.seq = seq;
        f.payload.assign(payload.begin(), payload.end());
        // Keep the deque ordered by seq so pop() is a front test rather than a scan.
        auto it = m_frames.begin();
        while (it != m_frames.end() && isNewer(seq, it->seq))
            ++it;
        m_frames.insert(it, std::move(f));
        while (m_frames.size() > m_maxDepth) {
            // Overflow drops the OLDEST. If that was the playhead, resynchronise onto the new front
            // rather than concealing a run of frames we deliberately threw away.
            m_frames.erase(m_frames.begin());
            if (!m_frames.empty())
                m_playhead = m_frames.front().seq;
        }
    }

    // Produce the next frame. `out` receives the payload on Pop::Ok (a view into buffer-owned
    // storage valid until the next push/pop).
    Pop pop(std::vector<uint8_t>& out) {
        if (!m_started) {
            if (m_frames.size() < m_targetDepth)
                return Pop::Empty; // still prefilling
            m_started = true;
            m_playhead = m_frames.front().seq;
        }
        if (m_frames.empty()) {
            // Underrun. Conceal a bounded run, then stall (and re-prefill) rather than inventing
            // speech indefinitely.
            if (m_concealRun >= kMaxVoiceConcealFrames) {
                m_started = false;
                m_concealRun = 0;
                return Pop::Empty;
            }
            ++m_concealRun;
            ++m_playhead;
            return Pop::Conceal;
        }
        if (m_frames.front().seq == m_playhead) {
            out = std::move(m_frames.front().payload);
            m_frames.erase(m_frames.begin());
            ++m_playhead;
            m_concealRun = 0;
            return Pop::Ok;
        }
        // A hole: the frame at the playhead is missing but later frames are held. Conceal one frame
        // and advance — the held frames stay in order.
        ++m_concealRun;
        ++m_playhead;
        return Pop::Conceal;
    }

    void reset() noexcept {
        m_frames.clear();
        m_started = false;
        m_playhead = 0;
        m_concealRun = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return m_frames.size();
    }
    [[nodiscard]] bool started() const noexcept {
        return m_started;
    }
    [[nodiscard]] uint32_t targetDepth() const noexcept {
        return m_targetDepth;
    }
    [[nodiscard]] uint32_t maxDepth() const noexcept {
        return m_maxDepth;
    }

    // 3 frames = 60 ms of de-jitter, on top of whatever the transport already smooths. Enough for
    // ordinary internet jitter; small enough that a call still feels conversational.
    static constexpr uint32_t kDefaultTargetDepth = 3;
    static constexpr uint32_t kDefaultMaxDepth = 12; // 240 ms; beyond this, latency is the worse fault
    static constexpr uint32_t kHardMaxDepth = 64;

    // Half-window sequence comparisons (uint16 wrap-safe).
    [[nodiscard]] static bool isNewer(uint16_t a, uint16_t b) noexcept {
        return static_cast<uint16_t>(a - b) != 0u && static_cast<uint16_t>(a - b) < 0x8000u;
    }
    [[nodiscard]] static bool isNewerOrEqual(uint16_t a, uint16_t b) noexcept {
        return static_cast<uint16_t>(a - b) < 0x8000u;
    }

  private:
    std::vector<VoiceFrame> m_frames; // ordered by seq, oldest first; bounded by m_maxDepth
    uint32_t m_targetDepth{kDefaultTargetDepth};
    uint32_t m_maxDepth{kDefaultMaxDepth};
    uint16_t m_playhead{0};
    int m_concealRun{0};
    bool m_started{false};
};

} // namespace fl
