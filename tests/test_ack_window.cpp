// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/AckWindow.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

using fl::ackAdvance;
using fl::ackReceived;
using fl::kAckWindowBits;

TEST_CASE("ackReceived: high-water and future ticks", "[ack_window]") {
    // The high-water tick itself is implicitly decoded regardless of the mask.
    CHECK(ackReceived(100u, 0u, 100u));
    // A tick past the high-water mark is not yet acked.
    CHECK_FALSE(ackReceived(100u, 0xFFFFFFFFu, 101u));
    CHECK_FALSE(ackReceived(100u, 0xFFFFFFFFu, 200u));
}

TEST_CASE("ackReceived: in-window bit lookup", "[ack_window]") {
    const uint64_t H = 100u;
    // age 1 -> bit 0, age 32 -> bit 31.
    CHECK(ackReceived(H, 0b1u, H - 1u));            // bit 0 set -> tick 99 decoded
    CHECK_FALSE(ackReceived(H, 0b1u, H - 2u));      // bit 1 clear -> tick 98 not decoded
    CHECK(ackReceived(H, 1u << 31, H - 32u));       // bit 31 set -> tick 68 decoded
    CHECK_FALSE(ackReceived(H, 1u << 31, H - 31u)); // bit 30 clear

    // A specific gap in an otherwise-full mask: every in-window tick decoded except tick H-3.
    const uint32_t mask = 0xFFFFFFFFu & ~(1u << 2); // bit 2 (age 3) clear
    CHECK(ackReceived(H, mask, H - 1u));
    CHECK(ackReceived(H, mask, H - 2u));
    CHECK_FALSE(ackReceived(H, mask, H - 3u)); // the one dropped tick
    CHECK(ackReceived(H, mask, H - 4u));
}

TEST_CASE("ackReceived: out-of-window ticks assumed decoded", "[ack_window]") {
    // age > 32: outside the reported window, assume decoded (retention force-full is the backstop).
    // This is the branch that keeps steady-state deltas flowing with a sparse/zero mask.
    const uint64_t H = 1000u;
    CHECK(ackReceived(H, 0u, H - 33u)); // age 33 -> true even with an empty mask
    CHECK(ackReceived(H, 0u, H - 500u));
    CHECK(ackReceived(H, 0u, 0u));
    // The window boundary: age 32 is still consulted (in-window), age 33 is assumed.
    CHECK_FALSE(ackReceived(H, 0u, H - 32u)); // in-window, bit clear -> not decoded
    CHECK(ackReceived(H, 0u, H - 33u));       // out-of-window -> decoded
}

TEST_CASE("ackAdvance: no previous high-water seeds an empty mask", "[ack_window]") {
    // Before the first snapshot there is no oldH; only newH (implicitly decoded) is known.
    CHECK(ackAdvance(0u, 0u, 500u, /*hadPrev=*/false) == 0u);
}

TEST_CASE("ackAdvance: consecutive ticks fill the mask", "[ack_window]") {
    uint32_t mask = 0u;
    // Receive 10 -> 11 -> 12 with no prior mask; each delta is 1.
    mask = ackAdvance(mask, 10u, 11u, true); // marks tick 10 -> bit 0
    CHECK(mask == 0b1u);
    mask = ackAdvance(mask, 11u, 12u, true); // marks tick 11 -> bit 0; tick 10 shifts to bit 1
    CHECK(mask == 0b11u);
    mask = ackAdvance(mask, 12u, 13u, true);
    CHECK(mask == 0b111u);
}

TEST_CASE("ackAdvance: a gap leaves a clear bit", "[ack_window]") {
    // Receive tick 10 then 12 (11 dropped): delta 2 shifts the old mask by 2 and marks only oldH.
    uint32_t mask = ackAdvance(0u, 9u, 10u, true); // bit 0 = tick 9
    CHECK(mask == 0b1u);
    mask = ackAdvance(mask, 10u, 12u, true); // delta 2: mark tick 10 (bit 1), tick 11 (bit 0) stays clear
    CHECK(mask == 0b110u);
    // Verify semantics against ackReceived at the new high-water 12.
    CHECK(ackReceived(12u, mask, 10u));       // bit 1 -> decoded
    CHECK_FALSE(ackReceived(12u, mask, 11u)); // bit 0 clear -> dropped
    CHECK(ackReceived(12u, mask, 12u));       // high-water
}

TEST_CASE("ackAdvance: window boundary at delta 32 has no undefined shift", "[ack_window]") {
    // delta == 32: oldH is the last in-window tick (bit 31); the old mask rolls fully out. Must not
    // execute `uint32_t << 32` (UB) — the helper special-cases it. This is the UBSan boundary.
    const uint32_t mask = ackAdvance(0xFFFFFFFFu, 100u, 132u, true);
    CHECK(mask == (1u << 31));
    CHECK(ackReceived(132u, mask, 100u)); // oldH decoded, age 32
    CHECK_FALSE(ackReceived(132u, mask, 101u));

    // delta > 32: the whole window rolls out; only the new high-water is known.
    CHECK(ackAdvance(0xFFFFFFFFu, 100u, 133u, true) == 0u);
    CHECK(ackAdvance(0xFFFFFFFFu, 100u, 1000u, true) == 0u);
}

TEST_CASE("AckWindow: window size is 32", "[ack_window]") {
    CHECK(kAckWindowBits == 32);
}
