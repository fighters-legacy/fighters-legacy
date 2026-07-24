// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "voice/RadioDsp.h"

#include <cmath>
#include <numbers>
#include <vector>

using namespace fl;

namespace {

constexpr int kRate = 48000;

std::vector<int16_t> tone(float hz, int samples = kRate / 10, float amp = 0.5f) {
    std::vector<int16_t> pcm(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kRate);
        pcm[static_cast<std::size_t>(i)] =
            static_cast<int16_t>(amp * 32767.f * std::sin(2.f * std::numbers::pi_v<float> * hz * t));
    }
    return pcm;
}

// RMS of the settled part of the buffer — the first few ms are filter transient, not signal.
float settledRms(const std::vector<int16_t>& pcm) {
    const std::size_t skip = pcm.size() / 4;
    double acc = 0.0;
    std::size_t n = 0;
    for (std::size_t i = skip; i < pcm.size(); ++i) {
        const double v = static_cast<double>(pcm[i]) / 32768.0;
        acc += v * v;
        ++n;
    }
    return n ? static_cast<float>(std::sqrt(acc / static_cast<double>(n))) : 0.f;
}

RadioProfile quietProfile() {
    RadioProfile p;
    p.noiseLevel = 0.f; // isolate the filter response from the hiss floor
    p.drive = 1.f;
    p.outputGain = 1.f;
    return p;
}

} // namespace

TEST_CASE("radio filter is a no-op until configured", "[voice][dsp]") {
    RadioFilter f;
    REQUIRE_FALSE(f.configured());
    auto pcm = tone(1000.f);
    const auto before = pcm;
    // A caller must be able to run the filter unconditionally without branching on setup state.
    f.process(pcm);
    REQUIRE(pcm == before);
}

TEST_CASE("radio filter passes the communications band and rejects outside it", "[voice][dsp]") {
    const auto profile = quietProfile();

    auto measure = [&](float hz) {
        RadioFilter f;
        f.configure(profile, kRate);
        auto pcm = tone(hz);
        f.process(pcm);
        return settledRms(pcm);
    };

    const float lowCut = measure(80.f);    // below the 300 Hz corner
    const float inBand = measure(1000.f);  // squarely in a voice band
    const float highCut = measure(9000.f); // well above the 3 kHz corner

    // Band-limiting is what makes a radio sound like a radio; if the band passes everything, the
    // effect is inaudible and #925 has done nothing.
    REQUIRE(inBand > 0.2f);
    REQUIRE(lowCut < inBand * 0.25f);
    REQUIRE(highCut < inBand * 0.25f);
}

TEST_CASE("radio filter compresses rather than hard-clipping", "[voice][dsp]") {
    RadioProfile p = quietProfile();
    p.drive = 4.f;
    RadioFilter f;
    f.configure(p, kRate);
    auto pcm = tone(1000.f, kRate / 10, /*amp=*/0.9f);
    f.process(pcm);

    // A soft clipper must not produce full-scale square edges — that reads as digital distortion,
    // not as a compressed channel. Peaks stay below full scale, and the signal stays non-trivial.
    int16_t peak = 0;
    for (const int16_t s : pcm)
        peak = std::max<int16_t>(peak, static_cast<int16_t>(std::abs(static_cast<int>(s))));
    REQUIRE(peak > 8000);
    REQUIRE(peak < 32767);
    REQUIRE(settledRms(pcm) > 0.2f);
}

TEST_CASE("radio filter adds hiss when a net asks for it", "[voice][dsp]") {
    RadioProfile p;
    p.noiseLevel = 0.05f;
    p.drive = 1.f;
    RadioFilter f;
    f.configure(p, kRate);
    std::vector<int16_t> silence(static_cast<std::size_t>(kRate / 10), 0);
    f.process(silence);
    // A dead-silent radio channel does not sound like a radio; a faint carrier hiss is the cue that
    // the receiver is on.
    REQUIRE(settledRms(silence) > 0.01f);
}

TEST_CASE("radio filter output is deterministic and byte-stable", "[voice][dsp]") {
    RadioProfile p;
    p.noiseLevel = 0.05f; // the noise path is the only place non-determinism could creep in

    RadioFilter a;
    RadioFilter b;
    a.configure(p, kRate);
    b.configure(p, kRate);
    auto pa = tone(700.f);
    auto pb = pa;
    a.process(pa);
    b.process(pb);
    // Two machines must synthesise the identical squelch and the identical hiss (the
    // SfxBuiltinSounds contract: a fixed hash, never rand() or a clock).
    REQUIRE(pa == pb);

    // reset() must return the filter to its exact initial state, not merely clear the biquads.
    a.reset();
    auto pc = tone(700.f);
    a.process(pc);
    REQUIRE(pc == pa);
}

TEST_CASE("radio filter rejects a nonsense sample rate instead of misbehaving", "[voice][dsp]") {
    RadioFilter f;
    f.configure(RadioProfile{}, 0);
    REQUIRE_FALSE(f.configured());
    auto pcm = tone(1000.f);
    const auto before = pcm;
    f.process(pcm);
    REQUIRE(pcm == before);
}

TEST_CASE("radio cues are non-empty, bounded, and byte-stable", "[voice][dsp]") {
    const auto click = radioClickPcm(kRate);
    const auto squelch = radioSquelchPcm(kRate);

    REQUIRE_FALSE(click.empty());
    REQUIRE_FALSE(squelch.empty());
    // The click must never be long enough to mask the first syllable it announces.
    REQUIRE(click.size() <= static_cast<std::size_t>(kRate / 40));  // <= 25 ms
    REQUIRE(squelch.size() <= static_cast<std::size_t>(kRate / 5)); // <= 200 ms
    REQUIRE(squelch.size() > click.size());

    // Deterministic: same input, same PCM, on every machine.
    REQUIRE(radioClickPcm(kRate) == click);
    REQUIRE(radioSquelchPcm(kRate) == squelch);

    // Audible, not silence.
    REQUIRE(settledRms(click) > 0.001f);
    REQUIRE(settledRms(squelch) > 0.001f);
}

TEST_CASE("radio cues scale with the sample rate and reject an invalid one", "[voice][dsp]") {
    REQUIRE(radioClickPcm(24000).size() * 2 == radioClickPcm(48000).size());
    REQUIRE(radioClickPcm(0).empty());
    REQUIRE(radioSquelchPcm(-1).empty());
}
