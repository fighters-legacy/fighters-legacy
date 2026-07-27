// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IClock.h"
#include "loop/TimeRate.h"
#include "net/SnapshotCodec.h"
#include "render/RenderSnapshot.h"
#include "replay/ReplayReader.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// ReplayPlayer (#41) — drives a `.flrep` into the client's existing presentation path.
//
// The design decision this rests on (plan D7): playback publishes RenderSnapshots through the SAME
// SimRenderBridge the network client publishes through, so the renderer, the cameras, the HUD and
// the terrain streamer cannot tell a replay from a live session and need no replay-awareness at all.
// A replay session is therefore a session with no server and no socket, not a second renderer.
//
// Timing comes from the FILE's tickRateHz, never a constant: a recording made at a different tick
// rate must play at the speed it was flown, and a hardcoded 60 would silently stretch or compress it.
// Speed control reuses fl::TimeRate rather than inventing a vocabulary -- the pause/quarter/half/
// normal/double set #41 asks for is already there and already means "multiplier on sim time".
//
// Scrubbing is the one operation the engine has no precedent for, because a simulation only ever
// moves forward. It is not solved by running backwards: seek to the keyframe at or before the target
// (the reader decompresses exactly that one chunk) and roll forward, rebuilding the delta cache on
// the way. That is why the recorder emits keyframes on a cadence at all.

namespace fl {

class ReplayPlayer {
  public:
    // Open a replay and position at its first tick. False on any refusal (a corrupt file, a newer
    // format major); the reason is in lastError().
    bool open(const std::filesystem::path& path);
    void close();

    [[nodiscard]] bool isOpen() const noexcept {
        return m_open;
    }
    [[nodiscard]] const ReplayHeader& header() const noexcept {
        return m_reader.header();
    }
    [[nodiscard]] const ReplaySections& sections() const noexcept {
        return m_reader.sections();
    }
    [[nodiscard]] const std::string& lastError() const noexcept {
        return m_lastError;
    }

    // Advance playback by `dtSeconds` of wall time and, if the tick cursor moved, decode forward and
    // fill `out` with the current world. Returns true when `out` was written (i.e. a new tick is
    // ready to publish). Paused playback returns false without touching `out`, which is what keeps a
    // paused photo-mode frame perfectly still.
    bool update(double dtSeconds, RenderSnapshot& out);

    // Force the current tick into `out` without advancing -- used right after a seek so the display
    // updates while paused.
    bool present(RenderSnapshot& out);

    // Setting a playing rate also records it as the resume rate, so pausing by ANY path -- this
    // setter, the key, or photo mode -- comes back at the speed the viewer was watching rather than
    // snapping to 1x.
    void setRate(TimeRate rate) noexcept {
        if (rate != TimeRate::Paused)
            m_resumeRate = rate;
        m_rate = rate;
    }
    [[nodiscard]] TimeRate rate() const noexcept {
        return m_rate;
    }
    void togglePause() noexcept {
        if (m_rate == TimeRate::Paused) {
            m_rate = m_resumeRate;
        } else {
            m_resumeRate = m_rate;
            m_rate = TimeRate::Paused;
        }
    }
    [[nodiscard]] bool paused() const noexcept {
        return m_rate == TimeRate::Paused;
    }

    // Seek to an absolute tick (clamped to the recording) or by a signed number of SECONDS. Both
    // land on the keyframe at or before the target and roll forward to it, so the world is complete
    // rather than a delta applied to nothing.
    bool seekToTick(uint64_t tick);
    bool seekBySeconds(double deltaSeconds);
    // 0 = start, 1 = end. The scrub bar's own vocabulary.
    bool seekToFraction(double fraction);

    [[nodiscard]] uint64_t currentTick() const noexcept {
        return m_currentTick;
    }
    [[nodiscard]] uint64_t firstTick() const noexcept {
        return m_reader.firstTick();
    }
    [[nodiscard]] uint64_t lastTick() const noexcept {
        return m_reader.lastTick();
    }
    [[nodiscard]] double elapsedSeconds() const noexcept;
    [[nodiscard]] double durationSeconds() const noexcept {
        return m_reader.durationSeconds();
    }
    // 0..1 through the recording; 0 when the recording has no length.
    [[nodiscard]] double progress() const noexcept;
    [[nodiscard]] bool atEnd() const noexcept {
        return m_atEnd;
    }

    // The keyframe ticks a scrub can land on -- what the transport bar draws as its ticks, so the
    // marks show where a seek will ACTUALLY land rather than being decoration.
    [[nodiscard]] std::vector<uint64_t> keyframeTicks() const {
        std::vector<uint64_t> out;
        out.reserve(m_reader.index().size());
        for (const auto& e : m_reader.index())
            out.push_back(e.tick);
        return out;
    }

    // Events on the most recently decoded tick (kills, chat, admin actions), in file order.
    [[nodiscard]] const std::vector<MatchEvent>& lastTickEvents() const noexcept {
        return m_lastEvents;
    }
    // participantId -> callsign, seeded from the roster section and extended by Join events as
    // playback reaches them (a participant who joined mid-recording is not in the section).
    [[nodiscard]] const std::unordered_map<uint32_t, std::string>& roster() const noexcept {
        return m_roster;
    }

  private:
    bool decodeCurrentInto(RenderSnapshot& out);
    bool advanceOneTick(); // decode the next tick into the entity cache
    void applyTick(const ReplayTick& tick);
    void resetCaches();

    ReplayReader m_reader;
    bool m_open{false};
    std::string m_lastError;

    TimeRate m_rate{TimeRate::Normal};
    TimeRate m_resumeRate{TimeRate::Normal};
    double m_tickAccumulator{0.0}; // fractional ticks carried between frames
    uint64_t m_currentTick{0};
    bool m_atEnd{false};
    bool m_havePresented{false};

    // The decoded world. A delta record carries no typeIndex/faction/gen, so playback keeps the same
    // per-entity cache a live client keeps -- rebuilt from the keyframe after every seek.
    struct Cached {
        EntityRenderEntry re;
        uint64_t lastSeenTick{0};
    };
    std::unordered_map<uint32_t, Cached> m_entities;
    std::unordered_map<uint32_t, QuantEntity> m_known; // typeIndex/faction/gen cache for deltas
    std::vector<MatchEvent> m_lastEvents;
    std::unordered_map<uint32_t, std::string> m_roster;
};

} // namespace fl
