// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "voice/VoiceActivity.h"

#include <vector>

using namespace fl;

namespace {
std::vector<int16_t> silence(std::size_t n = 960) {
    return std::vector<int16_t>(n, 0);
}
std::vector<int16_t> loud(std::size_t n = 960) {
    std::vector<int16_t> pcm(n);
    for (std::size_t i = 0; i < n; ++i)
        pcm[i] = (i % 2) ? 12000 : -12000; // RMS ~0.37, well over any sane VOX threshold
    return pcm;
}
} // namespace

TEST_CASE("voice gate PTT keys on the held key, not on level", "[voice]") {
    VoiceActivityGate g;
    g.setMode(VoiceKeyMode::PushToTalk);

    // Loud room noise with the key up must NOT transmit — that is the whole contract of PTT.
    auto r = g.evaluate(loud(), /*keyHeld=*/false);
    REQUIRE_FALSE(r.transmit);
    REQUIRE_FALSE(r.started);

    r = g.evaluate(silence(), /*keyHeld=*/true);
    REQUIRE(r.transmit);
    REQUIRE(r.started);
    REQUIRE_FALSE(r.ended);
    REQUIRE(g.isOpen());
}

TEST_CASE("voice gate emits exactly one start and one end per burst", "[voice]") {
    VoiceActivityGate g;
    g.setMode(VoiceKeyMode::PushToTalk);
    g.setHangoverFrames(0); // isolate the edges from the tail

    auto r = g.evaluate(loud(), true);
    REQUIRE(r.started);
    for (int i = 0; i < 4; ++i) {
        r = g.evaluate(loud(), true);
        REQUIRE(r.transmit);
        REQUIRE_FALSE(r.started); // start is an EDGE; a held key is one transmission, not many
    }
    r = g.evaluate(loud(), false);
    REQUIRE_FALSE(r.transmit);
    REQUIRE(r.ended);
    r = g.evaluate(loud(), false);
    REQUIRE_FALSE(r.ended); // and end is an edge too
}

TEST_CASE("voice gate VOX opens on level and holds through the hangover", "[voice]") {
    VoiceActivityGate g;
    g.setMode(VoiceKeyMode::Voice);
    g.setThreshold(0.05f);
    g.setHangoverFrames(3);

    REQUIRE_FALSE(g.evaluate(silence(), false).transmit);
    REQUIRE(g.evaluate(loud(), false).started);

    // A plosive gap mid-word must not chop the transmission in two.
    for (int i = 0; i < 3; ++i) {
        const auto r = g.evaluate(silence(), false);
        REQUIRE(r.transmit);
        REQUIRE_FALSE(r.ended);
    }
    const auto r = g.evaluate(silence(), false);
    REQUIRE_FALSE(r.transmit);
    REQUIRE(r.ended);
}

TEST_CASE("voice gate PTT key overrides VOX rather than locking it out", "[voice]") {
    VoiceActivityGate g;
    g.setMode(VoiceKeyMode::Voice);
    g.setThreshold(0.9f); // a threshold nothing will cross
    REQUIRE(g.evaluate(silence(), /*keyHeld=*/true).transmit);
}

TEST_CASE("voice gate Open mode always transmits", "[voice]") {
    VoiceActivityGate g;
    g.setMode(VoiceKeyMode::Open);
    REQUIRE(g.evaluate(silence(), false).started);
    REQUIRE(g.evaluate(silence(), false).transmit);
}

TEST_CASE("voice gate close() reports whether it closed an open transmission", "[voice]") {
    VoiceActivityGate g;
    g.setMode(VoiceKeyMode::PushToTalk);
    REQUIRE_FALSE(g.close()); // nothing was open
    g.evaluate(loud(), true);
    REQUIRE(g.isOpen());
    // Focus loss must produce the end-of-transmission marker, or the receiver's squelch never fires.
    REQUIRE(g.close());
    REQUIRE_FALSE(g.isOpen());
    REQUIRE_FALSE(g.close());
}

TEST_CASE("voice gate RMS is normalised and empty-safe", "[voice]") {
    REQUIRE(VoiceActivityGate::rms({}) == 0.f);
    REQUIRE(VoiceActivityGate::rms(silence()) == 0.f);
    const float r = VoiceActivityGate::rms(loud());
    REQUIRE(r > 0.3f);
    REQUIRE(r <= 1.0f);
}

TEST_CASE("voice key mode ordinal guard rejects out-of-range bytes", "[voice]") {
    REQUIRE(isVoiceKeyModeOrdinal(0));
    REQUIRE(isVoiceKeyModeOrdinal(2));
    REQUIRE_FALSE(isVoiceKeyModeOrdinal(3));
    REQUIRE_FALSE(isVoiceKeyModeOrdinal(255));
}
