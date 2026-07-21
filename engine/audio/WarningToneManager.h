// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IAudio.h"
#include "audio/OggDecoder.h" // DecodedPcm
#include "config/AudioSettings.h"

#include <cstdint>

namespace fl {

class ILogger;

// Cockpit warning tones (#957, part of Epic #586) — the audio counterpart to StallBuffet's haptics.
// A small, HEADLESS-TESTABLE state machine that turns two local flight predicates (stall, overspeed)
// into continuous head-locked audio cues, with entry/exit hysteresis so a predicate that flickers on
// the threshold does not chatter the tone on and off.
//
// Stall + overspeed are derived from the OWN aircraft's FlightState (no sensor/network dep). The RWR
// tones (#960) are driven by the peer's LEGITIMATE threat picture — the datalink/RWR strobes the
// server decided this peer detects — so cueing them leaks nothing the RWR did not honestly hear. Both
// families share one hysteresis + head-locked-source machine and are fully unit-testable through a
// tracking IAudio (no device).
//
// Like SfxBuiltinSounds, the tones are compiled-in and BYTE-STABLE (deterministic integer/float math,
// never rand()/time), so they sound in the zero-pack sandbox and a golden test can pin their shape.
enum class WarningTone {
    Stall,     // an intermittent horn (gated beep) — the classic stall warning
    Overspeed, // a steady high clacker — over the never-exceed speed / structural limit
    RwrSearch, // a slow low strobe chirp — an emitter is scanning you
    RwrLock,   // a steady mid lock tone — an emitter holds a firing-quality lock on you
    RwrLaunch, // a fast urgent warble — a radar-guided missile is guiding on you (#960)
};

// The RWR tone the caller wants THIS frame — the WORST hostile threat level in the peer's RWR picture
// (mirrors sensor::ThreatLevel, plus None). Only one RWR tone sounds at a time: the manager plays the
// tone for the highest level, escalates instantly, and de-escalates through the hold timer.
enum class RwrThreat : std::uint8_t {
    None = 0,
    Search = 1,
    Lock = 2,
    Launch = 3,
};

// Synthesise the looping PCM for one warning tone. Mono int16 at kWarningToneSampleRate; the buffer
// length is an integer number of tone/gate cycles so it loops seamlessly, and the loop seam sits in a
// silent (gated-off) region for the intermittent tone. Pure, never fails (non-empty buffer).
[[nodiscard]] DecodedPcm generateWarningTonePcm(WarningTone tone);

inline constexpr int kWarningToneSampleRate = 22050;

// The per-tick predicates driving the tones. The caller derives these from the own aircraft's state:
//   stall     ← FlightState::stalled
//   overspeed ← Mach beyond the model's never-exceed limit (or a structural over-speed)
//   rwr       ← the worst hostile level in the peer's RWR picture (None when nothing threatens)
// inFlight gates the whole manager: when false (menu, non-cockpit, no own entity) every tone is
// silenced immediately, bypassing the hold-timer so it never lingers into a menu.
struct WarningToneInputs {
    bool inFlight = false;
    bool stall = false;
    bool overspeed = false;
    RwrThreat rwr = RwrThreat::None;
};

class WarningToneManager {
  public:
    // audio/logger must outlive the manager. A null IAudio is tolerated (headless / CI): every method
    // becomes a no-op, so callers need no #ifdef. Sources are created lazily on first activation.
    bool init(IAudio* audio, ILogger* logger);
    void shutdown();

    // Advance the state machine one frame. The stall/overspeed gain follows master*sfx and the RWR
    // gain master*rwr, both live so a mid-session slider change applies at once. dt drives the exit
    // hysteresis.
    void update(const WarningToneInputs& in, const AudioSettings& settings, float dt);

    // Introspection for tests / HUD.
    [[nodiscard]] bool stallActive() const noexcept {
        return m_stall.active;
    }
    [[nodiscard]] bool overspeedActive() const noexcept {
        return m_overspeed.active;
    }
    // The RWR tone currently sounding (RwrThreat::None when silent) — after hysteresis, so it may lag
    // a just-cleared threat by up to kRwrHoldSeconds.
    [[nodiscard]] RwrThreat rwrActive() const noexcept {
        return m_rwr.level;
    }

    // A tone stays audible this long after its predicate clears, then stops — debounces threshold
    // flicker without an audible lag on genuine recovery. RWR de-escalation gets its own slightly
    // longer hold (a threat that drops for a frame should not silence the tone).
    static constexpr float kHoldSeconds = 0.5f;
    static constexpr float kRwrHoldSeconds = 0.75f;

  private:
    struct Channel {
        AudioSourceId src{0};
        AudioBufferId buf{0};
        bool active{false};   // currently sounding
        float holdTimer{0.f}; // seconds of remaining hold after the predicate cleared
    };

    // The single RWR voice. One source whose buffer is swapped to match the sounding level; the three
    // tone buffers are uploaded lazily. `level` is the level after hysteresis; `sounding` is the tone
    // currently loaded on the source.
    struct RwrChannel {
        AudioSourceId src{0};
        AudioBufferId buf[3]{0, 0, 0}; // indexed by (level - 1): Search / Lock / Launch
        RwrThreat level{RwrThreat::None};
        RwrThreat sounding{RwrThreat::None};
        float holdTimer{0.f};
    };

    void driveChannel(Channel& ch, WarningTone tone, bool want, float gain, float dt);
    void driveRwr(RwrThreat want, float gain, float dt);

    IAudio* m_audio{nullptr};
    ILogger* m_logger{nullptr};
    Channel m_stall;
    Channel m_overspeed;
    RwrChannel m_rwr;
};

} // namespace fl
