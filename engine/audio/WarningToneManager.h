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
// Only stall + overspeed live here — RWR / missile-lock tones are split to #960 because they depend
// on the Epic F sensor/threat picture. This channel is deliberately self-contained: its inputs are
// booleans the caller derives from the OWN aircraft's FlightState, so it needs nothing from the
// sensor or network layers and is fully unit-testable through a tracking IAudio (no device).
//
// Like SfxBuiltinSounds, the tones are compiled-in and BYTE-STABLE (deterministic integer/float math,
// never rand()/time), so they sound in the zero-pack sandbox and a golden test can pin their shape.
enum class WarningTone {
    Stall,     // an intermittent horn (gated beep) — the classic stall warning
    Overspeed, // a steady high clacker — over the never-exceed speed / structural limit
};

// Synthesise the looping PCM for one warning tone. Mono int16 at kWarningToneSampleRate; the buffer
// length is an integer number of tone/gate cycles so it loops seamlessly, and the loop seam sits in a
// silent (gated-off) region for the intermittent tone. Pure, never fails (non-empty buffer).
[[nodiscard]] DecodedPcm generateWarningTonePcm(WarningTone tone);

inline constexpr int kWarningToneSampleRate = 22050;

// The per-tick predicates driving the tones. The caller derives these from the own aircraft's state:
//   stall     ← FlightState::stalled
//   overspeed ← Mach beyond the model's never-exceed limit (or a structural over-speed)
// inFlight gates the whole manager: when false (menu, non-cockpit, no own entity) every tone is
// silenced immediately, bypassing the hold-timer so it never lingers into a menu.
struct WarningToneInputs {
    bool inFlight = false;
    bool stall = false;
    bool overspeed = false;
};

class WarningToneManager {
  public:
    // audio/logger must outlive the manager. A null IAudio is tolerated (headless / CI): every method
    // becomes a no-op, so callers need no #ifdef. Sources are created lazily on first activation.
    bool init(IAudio* audio, ILogger* logger);
    void shutdown();

    // Advance the state machine one frame. gain follows master*sfx every frame so a mid-session volume
    // change is applied live. dt seconds drives the exit hysteresis.
    void update(const WarningToneInputs& in, const AudioSettings& settings, float dt);

    // Introspection for tests / HUD.
    [[nodiscard]] bool stallActive() const noexcept {
        return m_stall.active;
    }
    [[nodiscard]] bool overspeedActive() const noexcept {
        return m_overspeed.active;
    }

    // A tone stays audible this long after its predicate clears, then stops — debounces threshold
    // flicker without an audible lag on genuine recovery.
    static constexpr float kHoldSeconds = 0.5f;

  private:
    struct Channel {
        AudioSourceId src{0};
        AudioBufferId buf{0};
        bool active{false};   // currently sounding
        float holdTimer{0.f}; // seconds of remaining hold after the predicate cleared
    };

    void driveChannel(Channel& ch, WarningTone tone, bool want, float gain, float dt);

    IAudio* m_audio{nullptr};
    ILogger* m_logger{nullptr};
    Channel m_stall;
    Channel m_overspeed;
};

} // namespace fl
