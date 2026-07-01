// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace fl {

// Selective-ack window for snapshot identity confirmation (#566).
//
// #517 drives snapshot full-vs-delta off a single per-peer high-water-mark ack: the highest
// MsgWorldSnapshot tick the client echoes back. That cannot distinguish "the client decoded the full
// sent at tick S" from "the client received a later tick >= S but missed S", so a full dropped on a
// couple of consecutive ticks could be mis-confirmed and the entity briefly turns invisible.
//
// This adds a 32-bit selective-ack bitmask (à la TCP SACK) alongside the high-water tick. The client
// reports, for the ticks just below its high-water mark, which ones it actually DECODED; the server
// then confirms delivery of the SPECIFIC tick a full was sent in rather than a high-water mark.
//
// Mask convention: given a high-water tick H, bit b (0..31) means "tick H - 1 - b was decoded". The
// high-water tick H itself is implicitly decoded (it is the tick being echoed).
constexpr int kAckWindowBits = 32;

// Server side: has the peer decoded snapshot `tick`, given its high-water `ackedTick` and `ackMask`?
//   * tick > ackedTick            -> not yet acked (future) -> false
//   * tick == ackedTick           -> the high-water tick is implicitly decoded -> true
//   * age in [1, 32]              -> consult bit (age - 1)
//   * age > 32 (out of window)    -> assume decoded. Older ticks fall outside the reported window; the
//                                    per-entity kSnapshotRetentionTicks force-full is the backstop, and
//                                    this matches the pre-#566 high-water semantics for old ticks.
inline bool ackReceived(uint64_t ackedTick, uint32_t ackMask, uint64_t tick) noexcept {
    if (tick > ackedTick)
        return false;
    if (tick == ackedTick)
        return true;
    const uint64_t age = ackedTick - tick; // >= 1
    if (age > static_cast<uint64_t>(kAckWindowBits))
        return true;
    return ((ackMask >> static_cast<uint32_t>(age - 1u)) & 1u) != 0u;
}

// Client side: advance the decoded-tick mask when the high-water tick moves oldH -> newH (newH > oldH).
// The just-superseded high-water `oldH` was decoded, so it becomes a set bit in the new frame; every
// prior bit shifts up by delta. A jump of more than the window rolls the whole mask out (only newH,
// implicitly decoded, is known). `hadPrev` is false before the first snapshot, when there is no oldH.
//
// The delta == 32 case is special-cased to avoid the `uint32_t << 32` undefined behaviour: oldH is the
// last in-window tick (bit 31) and the old mask shifts fully out.
inline uint32_t ackAdvance(uint32_t mask, uint64_t oldH, uint64_t newH, bool hadPrev) noexcept {
    const uint64_t delta = newH - oldH;
    if (!hadPrev || delta > static_cast<uint64_t>(kAckWindowBits))
        return 0u;
    if (delta == static_cast<uint64_t>(kAckWindowBits))
        return 1u << 31;
    const auto d = static_cast<uint32_t>(delta);
    return (mask << d) | (1u << (d - 1u));
}

} // namespace fl
