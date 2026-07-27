// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ReplayStateHash (#644, plan D8) — a per-tick fingerprint of the whole simulated world.
//
// The engine had serial-equivalence tests (byte-identical per-peer buffers across worker counts) but
// no primitive that answers "is the SIM producing the same world twice". This is that primitive, and
// it has two jobs: it is the record-vs-replay fidelity check (#644's gate), and it is the sim-drift
// alarm the roadmap asks for -- anything that makes the tick nondeterministic (a thread race, an
// unseeded RNG, a float path that depends on evaluation order) changes the stream.
//
// Two properties make it a usable gate rather than a flaky one:
//
//   1. It hashes the QUANTIZED integer domain, not the raw doubles. Quantization is lossy, so a
//      record that goes through the codec and comes back can never equal its input in float space --
//      but it must be exactly equal after quantization, and that is the property worth gating. It
//      also removes float-order ambiguity between workers and platforms, the same reasoning that
//      makes Detection.cpp's rollPasses compare integers.
//   2. Records are folded in ASCENDING ENTITY INDEX, which the caller must supply. Every producer in
//      this engine already has a deterministic order available; a hash over a map's iteration order
//      would fail randomly and teach everyone to ignore it.
//
// FNV-1a 64 for the same reason the rest of the codebase uses it: it is short, it is a pure integer
// function of the bytes, and it is not a security primitive -- nothing here defends against a
// deliberately-collided replay, only against drift.

#include "Quantization.h"
#include "SnapshotCodec.h"

#include <cstddef>
#include <cstdint>

namespace fl {

inline constexpr uint64_t kFnv1a64Offset = 1469598103934665603ull;
inline constexpr uint64_t kFnv1a64Prime = 1099511628211ull;

inline void hashFold(uint64_t& h, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (8 * i)) & 0xFFull;
        h *= kFnv1a64Prime;
    }
}

// Fold one entity's quantized state into `h`. Position is folded relative to the entity's own shared
// grid origin -- exactly what the codec transmits -- so the hash is a function of what a replay can
// actually reproduce, not of a world offset that never crosses the wire.
inline void hashQuantEntity(uint64_t& h, const QuantEntity& e) noexcept {
    double origin[3];
    originForPos(e.pos, origin);

    hashFold(h, e.idx);
    hashFold(h, e.gen);
    hashFold(h, e.typeIndex);
    hashFold(h, e.factionIndex);

    for (int i = 0; i < 3; ++i) {
        // The codec quantizes the offset from the grid origin at kPosStepM; the origin itself is a
        // multiple of kOriginGridM and is folded so two entities in different cells never collide.
        hashFold(h, static_cast<uint64_t>(static_cast<int64_t>(origin[i] / kOriginGridM)));
        hashFold(h, static_cast<uint64_t>(quantizeSigned(e.pos[i] - origin[i], kPosStepM, kPosBitsPerAxis)));
    }

    const SmallestThree q = encodeSmallestThree(e.quat, kQuatBits);
    hashFold(h, q.maxIdx);
    for (uint32_t c : q.comp)
        hashFold(h, c);

    for (int i = 0; i < 3; ++i)
        hashFold(h, quantizeRange(static_cast<double>(e.vel[i]), kVelMaxMps, kVelBits));

    // omega is own-record-only on the wire, so it is deliberately NOT part of the world fingerprint:
    // including it would make the hash depend on which peer was receiving, and a replay carries no
    // peer at all.

    hashFold(h, e.damageLevel);
    hashFold(h, e.engineFailFlags);
    hashFold(h, e.throttle);
    hashFold(h, e.fuelPct);
    hashFold(h, e.abEngaged ? 1u : 0u);
    hashFold(h, e.playerOwned ? 1u : 0u);
}

// Hash a whole tick. `ents` must be in ascending idx order; `count` is folded in so a truncated
// record list cannot hash equal to a complete one that happens to share a prefix.
[[nodiscard]] inline uint64_t hashTickState(uint64_t tick, const QuantEntity* ents, std::size_t count) noexcept {
    uint64_t h = kFnv1a64Offset;
    hashFold(h, tick);
    hashFold(h, static_cast<uint64_t>(count));
    for (std::size_t i = 0; i < count; ++i)
        hashQuantEntity(h, ents[i]);
    return h;
}

} // namespace fl
