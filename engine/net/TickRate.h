// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

// The server's simulation tick rate, and the ONE place ticks convert to wall time.
//
// WHY THIS EXISTS (#1075). MsgConnectAck has carried a `tickRateHz` field since the first handshake,
// and nothing honoured it: the struct defaulted it to 60, the send site hardcoded 60, the client
// never read it, and the rate was re-derived as a bare literal in at least five other places
// (`* 1000u / 60u` twice in WorldBroadcaster, once in the client's RTT readout, `1.0f / 60.0f` in the
// renderer and the camera, `dt * 60.0f` in the render-alpha clock). That is the #1050 defect class
// exactly — a field that exists, that a reader would reasonably believe is honoured, and that
// changing has no effect whatsoever.
//
// It is also inconsistent with the replay path, which gets this right: ReplayPlayer advances at THE
// FILE's tickRateHz and never a constant, precisely because a stored rate that is ignored produces a
// silently wrong playback speed.
//
// This makes the field HONEST, not configurable. The server still runs at 60 Hz and still sends 60 —
// physics, prediction, lag compensation and the scale gate all assume it, and a configurable server
// tick rate is a much larger change that is deliberately not in scope. What changes is that every
// consumer derives its conversion from the value that actually travelled, so the rate has one
// authority instead of six copies that can disagree.
//
// Pattern: fl::ServerUptime — one authority for a value, passed in rather than defaulted, because a
// second source of the same fact is the bug.
class TickRate {
  public:
    static constexpr uint8_t kDefaultHz = 60;

    constexpr TickRate() noexcept = default;

    // A zero rate is REJECTED rather than propagated: this value arrives from the wire, every
    // accessor below divides by it, and a hostile or broken server sending 0 would otherwise reach a
    // division by zero on the client. Falling back to the default keeps a malformed ack merely wrong
    // about the rate instead of fatal.
    constexpr explicit TickRate(uint8_t hz) noexcept : m_hz(hz == 0 ? kDefaultHz : hz) {}

    [[nodiscard]] constexpr uint8_t hz() const noexcept {
        return m_hz;
    }

    // Seconds per tick — the render-side extrapolation step (`velocity * alpha * dt`).
    [[nodiscard]] constexpr float dtSeconds() const noexcept {
        return 1.0f / static_cast<float>(m_hz);
    }

    // Tick count as milliseconds. The "Ping: N ms" readout and the per-peer metrics both want this,
    // and both used to spell it `* 1000u / 60u` in place.
    [[nodiscard]] constexpr uint64_t ticksToMs(uint64_t ticks) const noexcept {
        return ticks * 1000u / m_hz;
    }

    // Milliseconds as a tick count, rounded down. The inverse of ticksToMs, for a caller that has a
    // wall-clock budget and needs it in ticks.
    [[nodiscard]] constexpr uint64_t msToTicks(uint64_t ms) const noexcept {
        return ms * m_hz / 1000u;
    }

    // How far through the current tick a wall-clock delta is, clamped to [0,1]. The renderer's
    // interpolation alpha: the clamp matters because a frame that overran a whole tick must hold at
    // the tick boundary rather than extrapolate past the next snapshot it has not received.
    [[nodiscard]] constexpr float alphaFromSeconds(float seconds) const noexcept {
        const float a = seconds * static_cast<float>(m_hz);
        return a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
    }

    friend constexpr bool operator==(TickRate a, TickRate b) noexcept {
        return a.m_hz == b.m_hz;
    }
    friend constexpr bool operator!=(TickRate a, TickRate b) noexcept {
        return !(a == b);
    }

  private:
    uint8_t m_hz{kDefaultHz};
};

// The rate the server runs at and advertises. Named so a reader looking for "where is 60 decided"
// finds one answer; see the class comment for why it is not configurable.
inline constexpr TickRate kServerTickRate{TickRate::kDefaultHz};

} // namespace fl
