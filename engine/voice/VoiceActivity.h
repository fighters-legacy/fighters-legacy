// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace fl {

// How the microphone is keyed.
enum class VoiceKeyMode : uint8_t {
    PushToTalk = 0, // transmit only while a PTT key is held — the default, and the only sane one in a
                    // 128-player server: an open mic on a public net is a denial-of-service you
                    // inflict on your own team.
    Voice = 1,      // VOX: transmit while input energy is over a threshold, with a hangover tail
    Open = 2,       // always transmit (LAN / trusted small groups)
};

[[nodiscard]] inline bool isVoiceKeyModeOrdinal(uint8_t v) noexcept {
    return v <= static_cast<uint8_t>(VoiceKeyMode::Open);
}

// ---------------------------------------------------------------------------------------------
// Voice activity gate (#531)
// ---------------------------------------------------------------------------------------------
// Decides, per 20 ms frame, whether to transmit. Pure and deterministic — no clock, no device, no
// codec — so the whole keying policy is unit-testable without a microphone.
//
// Two properties earn their keep:
//   * HANGOVER. A gate that closes the instant energy drops below threshold clips the tail off
//     every word ("Fox thre-"). The hangover holds the gate open for a short tail after the last
//     frame over threshold, so a plosive gap mid-word does not chop the transmission into two.
//   * An explicit START/END edge. The transmission boundary is what the wire flags carry and what
//     the presentation layer (#925) turns into a PTT click and a squelch tail — deriving it from
//     "frames stopped arriving" on the receiving end would put the squelch a timeout late.
// ---------------------------------------------------------------------------------------------
class VoiceActivityGate {
  public:
    struct Result {
        bool transmit{false}; // send this frame
        bool started{false};  // this frame opens a transmission
        bool ended{false};    // this frame closes it (the last frame of the burst)
    };

    void setMode(VoiceKeyMode mode) noexcept {
        m_mode = mode;
    }
    [[nodiscard]] VoiceKeyMode mode() const noexcept {
        return m_mode;
    }

    // VOX threshold as a linear RMS amplitude in [0, 1]; ~0.02 is a quiet room, ~0.08 a noisy one.
    void setThreshold(float rms) noexcept {
        m_threshold = std::clamp(rms, 0.f, 1.f);
    }
    void setHangoverFrames(int frames) noexcept {
        m_hangoverFrames = std::clamp(frames, 0, 200);
    }

    // Evaluate one captured frame. `keyHeld` is the PTT key state (ignored in Voice/Open modes).
    Result evaluate(std::span<const int16_t> pcm, bool keyHeld) noexcept {
        bool want = false;
        switch (m_mode) {
        case VoiceKeyMode::PushToTalk:
            want = keyHeld;
            break;
        case VoiceKeyMode::Open:
            want = true;
            break;
        case VoiceKeyMode::Voice:
            // A held PTT key also opens a VOX gate: the key is always an override, never a lockout.
            want = keyHeld || rms(pcm) >= m_threshold;
            break;
        }

        if (want) {
            m_hangoverLeft = m_hangoverFrames;
        } else if (m_open && m_hangoverLeft > 0) {
            --m_hangoverLeft;
            want = true; // hold the tail
        }

        Result r;
        r.transmit = want;
        r.started = want && !m_open;
        r.ended = !want && m_open;
        m_open = want;
        return r;
    }

    // Force the gate shut (focus loss, entering a menu, device error). Returns true if this closed
    // an open transmission, so the caller can emit the end-of-transmission flag.
    bool close() noexcept {
        const bool wasOpen = m_open;
        m_open = false;
        m_hangoverLeft = 0;
        return wasOpen;
    }

    [[nodiscard]] bool isOpen() const noexcept {
        return m_open;
    }

    // Linear RMS of an int16 frame, normalised to [0, 1]. Also the mic level the settings UI shows.
    [[nodiscard]] static float rms(std::span<const int16_t> pcm) noexcept {
        if (pcm.empty())
            return 0.f;
        double acc = 0.0;
        for (const int16_t s : pcm) {
            const double v = static_cast<double>(s) / 32768.0;
            acc += v * v;
        }
        return static_cast<float>(std::sqrt(acc / static_cast<double>(pcm.size())));
    }

    // 10 frames = 200 ms. Long enough to bridge a mid-sentence breath, short enough that the net
    // is not held open after the speaker stops.
    static constexpr int kDefaultHangoverFrames = 10;
    static constexpr float kDefaultThreshold = 0.03f;

  private:
    VoiceKeyMode m_mode{VoiceKeyMode::PushToTalk};
    float m_threshold{kDefaultThreshold};
    int m_hangoverFrames{kDefaultHangoverFrames};
    int m_hangoverLeft{0};
    bool m_open{false};
};

} // namespace fl
