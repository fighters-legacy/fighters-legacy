// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "voice/VoiceJitterBuffer.h"

#include <vector>

using namespace fl;

namespace {
std::vector<uint8_t> frame(uint8_t tag) {
    return {tag, tag, tag};
}
} // namespace

TEST_CASE("voice jitter buffer prefills before playing", "[voice]") {
    VoiceJitterBuffer jb(3, 12);
    std::vector<uint8_t> out;

    // Below target depth the buffer must stay silent, or the first jitter spike underruns
    // immediately and the transmission opens with a stutter.
    jb.push(0, frame(0));
    REQUIRE(jb.pop(out) == VoiceJitterBuffer::Pop::Empty);
    jb.push(1, frame(1));
    REQUIRE(jb.pop(out) == VoiceJitterBuffer::Pop::Empty);
    jb.push(2, frame(2));
    REQUIRE(jb.pop(out) == VoiceJitterBuffer::Pop::Ok);
    REQUIRE(out == frame(0));
    REQUIRE(jb.started());
}

TEST_CASE("voice jitter buffer reorders late arrivals", "[voice]") {
    VoiceJitterBuffer jb(2, 12);
    std::vector<uint8_t> out;
    // Out-of-order arrival is the whole reason voice cannot reuse the input jitter buffer.
    jb.push(1, frame(1));
    jb.push(0, frame(0));
    REQUIRE(jb.pop(out) == VoiceJitterBuffer::Pop::Ok);
    REQUIRE(out == frame(0));
    REQUIRE(jb.pop(out) == VoiceJitterBuffer::Pop::Ok);
    REQUIRE(out == frame(1));
}

TEST_CASE("voice jitter buffer drops duplicates and frames behind the playhead", "[voice]") {
    VoiceJitterBuffer jb(1, 12);
    std::vector<uint8_t> out;
    jb.push(5, frame(5));
    jb.push(5, frame(5)); // duplicate
    REQUIRE(jb.size() == 1);
    REQUIRE(jb.pop(out) == VoiceJitterBuffer::Pop::Ok);

    jb.push(5, frame(9)); // already played: a late frame is a lost frame
    REQUIRE(jb.size() == 0);
}

TEST_CASE("voice jitter buffer conceals a hole and keeps later frames in order", "[voice]") {
    VoiceJitterBuffer jb(1, 12);
    std::vector<uint8_t> out;
    jb.push(0, frame(0));
    REQUIRE(jb.pop(out) == VoiceJitterBuffer::Pop::Ok);

    // Frame 1 never arrives; 2 does. The gap must surface as Conceal (run PLC), NOT as a silent
    // reorder that plays 2 in 1's slot.
    jb.push(2, frame(2));
    REQUIRE(jb.pop(out) == VoiceJitterBuffer::Pop::Conceal);
    REQUIRE(jb.pop(out) == VoiceJitterBuffer::Pop::Ok);
    REQUIRE(out == frame(2));
}

TEST_CASE("voice jitter buffer stops concealing after a bounded run", "[voice]") {
    VoiceJitterBuffer jb(1, 12);
    std::vector<uint8_t> out;
    jb.push(0, frame(0));
    REQUIRE(jb.pop(out) == VoiceJitterBuffer::Pop::Ok);

    // Concealment past a handful of frames is inventing speech that was never said.
    for (int i = 0; i < kMaxVoiceConcealFrames; ++i)
        REQUIRE(jb.pop(out) == VoiceJitterBuffer::Pop::Conceal);
    REQUIRE(jb.pop(out) == VoiceJitterBuffer::Pop::Empty);
    REQUIRE_FALSE(jb.started()); // re-prefills for the next burst
}

TEST_CASE("voice jitter buffer overflow drops the oldest, not the newest", "[voice]") {
    VoiceJitterBuffer jb(1, 4);
    std::vector<uint8_t> out;
    for (uint16_t i = 0; i < 8; ++i)
        jb.push(i, frame(static_cast<uint8_t>(i)));
    REQUIRE(jb.size() == 4);
    // In a live call the newest audio is the audio worth keeping.
    REQUIRE(jb.pop(out) == VoiceJitterBuffer::Pop::Ok);
    REQUIRE(out == frame(4));
}

TEST_CASE("voice jitter buffer sequence comparison is wrap-safe", "[voice]") {
    REQUIRE(VoiceJitterBuffer::isNewer(1, 0));
    REQUIRE_FALSE(VoiceJitterBuffer::isNewer(0, 1));
    // ~22 minutes of continuous speech wraps a uint16 at 20 ms/frame; it must not be a special case.
    REQUIRE(VoiceJitterBuffer::isNewer(0, 65535));
    REQUIRE_FALSE(VoiceJitterBuffer::isNewer(65535, 0));
    REQUIRE_FALSE(VoiceJitterBuffer::isNewer(7, 7));
    REQUIRE(VoiceJitterBuffer::isNewerOrEqual(7, 7));
}

TEST_CASE("voice jitter buffer plays across a sequence wrap", "[voice]") {
    VoiceJitterBuffer jb(1, 12);
    std::vector<uint8_t> out;
    jb.push(65535, frame(1));
    REQUIRE(jb.pop(out) == VoiceJitterBuffer::Pop::Ok);
    jb.push(0, frame(2));
    REQUIRE(jb.pop(out) == VoiceJitterBuffer::Pop::Ok);
    REQUIRE(out == frame(2));
}

TEST_CASE("voice jitter buffer refuses an over-long payload", "[voice]") {
    VoiceJitterBuffer jb(1, 12);
    std::vector<uint8_t> huge(kMaxVoicePayloadBytes + 1, 0x7F);
    jb.push(0, huge);
    REQUIRE(jb.size() == 0);
}
