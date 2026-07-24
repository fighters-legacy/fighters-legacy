// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace fl {

// ---------------------------------------------------------------------------------------------
// Radio presentation DSP (#925)
// ---------------------------------------------------------------------------------------------
// What separates a combat flight sim's radio from lobby voice chat is almost entirely
// PRESENTATION. The bytes are the same Opus either way; the difference is that a radio is
// band-limited, compressed, keyed with a click, and closes with a squelch tail — and that those
// cues tell you *when someone started and stopped talking* without anyone having to say "over".
//
// All of this lives here, pure and stateless-per-call, for two reasons:
//   1. It runs on BOTH human voice (VoiceMixer) and synthetic voice (ATC / AWACS / Epic O TTS via
//      VoiceCalloutManager). The epic's requirement is that a human and a synthetic transmission
//      are indistinguishable in presentation, and the only way to guarantee that is one
//      implementation, not two that drift.
//   2. It is testable without a device, a codec, or a network.
//
// The click and squelch are GENERATED, not sampled: deterministic, byte-stable procedural PCM (the
// SfxBuiltinSounds contract — a fixed hash, never rand() or a clock), so the radio sounds like a
// radio in the zero-content-pack sandbox and identical on every machine.
// ---------------------------------------------------------------------------------------------

// Per-net radio character. A server's net table carries `radioEffect` on/off; these are the knobs
// behind that flag, and a net that wants to sound different (a crisp intercom vs a distant HF net)
// varies them.
struct RadioProfile {
    float highpassHz{300.f};  // the low end a radio simply does not pass
    float lowpassHz{3000.f};  // classic communications bandwidth; above this is breath and cockpit noise
    float drive{2.2f};        // pre-clip gain — this is what makes a shouted call sound compressed
    float noiseLevel{0.015f}; // additive hiss floor, [0, 1]; 0 = a perfectly clean channel
    float outputGain{0.9f};   // trim after the clipper so drive does not raise perceived loudness
};

// A one-pole-per-stage biquad pair plus a soft clipper and a deterministic noise source. STATEFUL:
// one instance per stream, because filter memory carried across two different speakers would smear
// one voice's tail into the next.
class RadioFilter {
  public:
    void configure(const RadioProfile& profile, int sampleRate) noexcept;
    void reset() noexcept;

    // Filter in place. Safe to call with an unconfigured filter (no-op), so a caller need not branch.
    void process(std::span<int16_t> pcm) noexcept;

    [[nodiscard]] bool configured() const noexcept {
        return m_configured;
    }

  private:
    struct Biquad {
        float b0{1.f}, b1{0.f}, b2{0.f}, a1{0.f}, a2{0.f};
        float x1{0.f}, x2{0.f}, y1{0.f}, y2{0.f};
        [[nodiscard]] float step(float x) noexcept;
        void reset() noexcept;
    };
    Biquad m_hp;
    Biquad m_lp;
    RadioProfile m_profile;
    uint32_t m_noiseState{0x9E3779B9u}; // deterministic: the same input always yields the same output
    bool m_configured{false};
};

// Sample rate the cue generators produce at. Matches the voice codec so a cue can be queued on the
// same OpenAL source as the speech without a resample.
inline constexpr int kRadioCueSampleRate = 48000;

// The mic key-down click: a short filtered transient. ~15 ms.
[[nodiscard]] std::vector<int16_t> radioClickPcm(int sampleRate = kRadioCueSampleRate);

// The key-up squelch tail: a click followed by a short decaying noise burst. ~120 ms. This is the
// cue that tells a listener the transmission ENDED rather than merely paused — which is why the
// wire carries an explicit end-of-transmission marker instead of leaving it to a receive timeout.
[[nodiscard]] std::vector<int16_t> radioSquelchPcm(int sampleRate = kRadioCueSampleRate);

} // namespace fl
