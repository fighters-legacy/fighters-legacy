// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "voice/VoiceCodec.h"

#include <cmath>
#include <numbers>
#include <vector>

using namespace fl;

namespace {

// A 440 Hz tone at moderate level — periodic, so a correctly configured Opus round trip preserves
// its energy even though the codec is lossy.
std::vector<int16_t> tone(int samples, float hz = 440.f, float amp = 0.3f) {
    std::vector<int16_t> pcm(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kVoiceSampleRate);
        pcm[static_cast<std::size_t>(i)] =
            static_cast<int16_t>(amp * 32767.f * std::sin(2.f * std::numbers::pi_v<float> * hz * t));
    }
    return pcm;
}

float rms(const std::vector<int16_t>& pcm) {
    double acc = 0.0;
    for (const int16_t s : pcm) {
        const double v = static_cast<double>(s) / 32768.0;
        acc += v * v;
    }
    return pcm.empty() ? 0.f : static_cast<float>(std::sqrt(acc / static_cast<double>(pcm.size())));
}

} // namespace

TEST_CASE("voice codec factories produce usable encoder and decoder", "[voice]") {
    auto enc = createVoiceEncoder();
    auto dec = createVoiceDecoder();
    REQUIRE(enc != nullptr);
    REQUIRE(dec != nullptr);
    REQUIRE(voiceCodecVersion() != nullptr);
}

TEST_CASE("voice codec frame geometry is the fixed operating point", "[voice]") {
    // The frame geometry is a client-to-client contract the server never inspects; assert it so a
    // change to the constants is a deliberate, visible act.
    REQUIRE(kVoiceSampleRate == 48000);
    REQUIRE(kVoiceChannels == 1);
    REQUIRE(kVoiceFrameMs == 20);
    REQUIRE(kVoiceFrameSamples == 960);
}

TEST_CASE("voice codec round-trips a tone within the payload cap", "[voice]") {
    auto enc = createVoiceEncoder();
    auto dec = createVoiceDecoder();
    REQUIRE(enc);
    REQUIRE(dec);

    const auto pcm = tone(kVoiceFrameSamples);
    std::vector<uint8_t> packet(kMaxVoicePayloadBytes);
    std::vector<int16_t> out(kVoiceFrameSamples);

    // Opus needs a few frames to settle; run several and check the last.
    std::size_t n = 0;
    for (int i = 0; i < 10; ++i) {
        n = enc->encode(pcm, packet);
        REQUIRE(n > 0);
        REQUIRE(n <= kMaxVoicePayloadBytes);
        REQUIRE(dec->decode(std::span<const uint8_t>(packet.data(), n), out) == kVoiceFrameSamples);
    }
    // Energy is preserved to within a lossy codec's tolerance — not sample-exact, but not silence.
    REQUIRE(rms(out) > 0.05f);
}

TEST_CASE("voice decoder conceals a missing frame instead of failing", "[voice]") {
    auto enc = createVoiceEncoder();
    auto dec = createVoiceDecoder();
    REQUIRE(enc);
    REQUIRE(dec);

    const auto pcm = tone(kVoiceFrameSamples);
    std::vector<uint8_t> packet(kMaxVoicePayloadBytes);
    std::vector<int16_t> out(kVoiceFrameSamples);
    for (int i = 0; i < 5; ++i) {
        const std::size_t n = enc->encode(pcm, packet);
        REQUIRE(dec->decode(std::span<const uint8_t>(packet.data(), n), out) == kVoiceFrameSamples);
    }

    // An EMPTY payload is the loss-concealment request. It must still produce a full frame — that
    // is what keeps a dropped packet from becoming a hole in the audio clock.
    REQUIRE(dec->decode(std::span<const uint8_t>{}, out) == kVoiceFrameSamples);
}

TEST_CASE("voice decoder refuses an over-long payload before it reaches libopus", "[voice]") {
    auto dec = createVoiceDecoder();
    REQUIRE(dec);
    // These bytes crossed the network through a server that never inspected them; the length cap is
    // the one validation available, and it must happen on our side of the codec.
    std::vector<uint8_t> huge(kMaxVoicePayloadBytes + 1, 0xAB);
    std::vector<int16_t> out(kVoiceFrameSamples);
    REQUIRE(dec->decode(huge, out) == 0);
}

TEST_CASE("voice encoder rejects a wrong-sized frame", "[voice]") {
    auto enc = createVoiceEncoder();
    REQUIRE(enc);
    const auto shortFrame = tone(kVoiceFrameSamples / 2);
    std::vector<uint8_t> packet(kMaxVoicePayloadBytes);
    REQUIRE(enc->encode(shortFrame, packet) == 0);
}

TEST_CASE("voice encoder bitrate and loss settings are accepted", "[voice]") {
    auto enc = createVoiceEncoder();
    REQUIRE(enc);
    // Out-of-range values are clamped, not rejected — a bad config file must not silence the radio.
    enc->setBitrate(1);
    enc->setBitrate(10'000'000);
    enc->setExpectedPacketLoss(-5);
    enc->setExpectedPacketLoss(500);
    enc->reset();

    const auto pcm = tone(kVoiceFrameSamples);
    std::vector<uint8_t> packet(kMaxVoicePayloadBytes);
    REQUIRE(enc->encode(pcm, packet) > 0);
}
