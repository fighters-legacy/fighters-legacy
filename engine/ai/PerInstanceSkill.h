// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl::ai {

// ── Per-instance skill (#966/#971) ───────────────────────────────────────────
//
// The first PER-INSTANCE skill in the engine (today AiTuning is type-level and sensing-only). Two
// units of the same type, spawned from the same mission, roll DIFFERENT skill so a flight of bombers
// is not uniformly deadly or uniformly hapless. The roll is a deterministic function of a seed —
// mission seed XOR object id XOR seat index — so a replay (and a 1-vs-N-worker run) is byte-identical.
//
// Mirrors the `detectionHash`/`rollPasses` integer-RNG idiom (engine/sensor/Detection.cpp) rather than
// any float RNG, so there is no float-order ambiguity across platforms. Generalised beyond crew seats:
// plain mission AI can roll per-instance skill too (seatIndex 0).

// Mix a seed into a well-distributed 32-bit hash (odd-constant LCG + xorshift).
[[nodiscard]] inline uint32_t skillHash(uint64_t seed) noexcept {
    uint32_t h =
        static_cast<uint32_t>(seed) * 0x9E3779B1u + static_cast<uint32_t>(seed >> 32) * 0x85EBCA77u + 0x27D4EB2Fu;
    h = h * 1664525u + 1013904223u;
    h ^= h >> 15;
    h = h * 1664525u + 1013904223u;
    return h;
}

// Combine (mission seed, object id, seat index) into one skill seed.
[[nodiscard]] inline uint64_t skillSeed(uint64_t missionSeed, uint32_t objectId, uint32_t seatIndex) noexcept {
    return missionSeed ^ (static_cast<uint64_t>(objectId) * 0x9E3779B97F4A7C15ull) ^
           (static_cast<uint64_t>(seatIndex) * 0xC2B2AE3D27D4EB4Full);
}

// Roll a skill in [minSkill, maxSkill] from a seed (uniform via the high 24 bits — no float RNG).
// A degenerate range (max <= min) returns min, so a fixed skill needs no special-casing.
[[nodiscard]] inline float rollPerInstanceSkill(uint64_t seed, float minSkill, float maxSkill) noexcept {
    if (maxSkill <= minSkill)
        return minSkill;
    const uint32_t h = skillHash(seed);
    const float u = static_cast<float>(h >> 8) / static_cast<float>(1u << 24); // [0, 1)
    return minSkill + u * (maxSkill - minSkill);
}

} // namespace fl::ai
