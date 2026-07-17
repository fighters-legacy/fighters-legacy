// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/DamageDef.h"

#include <array>
#include <cstdint>

namespace fl {

// Live per-subsystem damage state (#675) — server-side, one per controlled entity. Born from a
// SubsystemSet; a subsystem fails once its own HP pool is exhausted, and a failure is LATCHED (it
// does not un-fail as HP is a one-way pool). Separate from the entity's main HP: the two damage
// pools run in parallel, so a subsystem can be knocked out before the airframe is dead.
struct SubsystemStateSet {
    std::array<float, kSubsystemCount> hp{};
    uint8_t failedMask{0}; // bit (1 << Subsystem) set when that subsystem has failed

    void init(const SubsystemSet& def) noexcept {
        for (int i = 0; i < kSubsystemCount; ++i)
            hp[i] = def.parts[i].hp;
        failedMask = 0;
    }
    [[nodiscard]] bool failed(Subsystem s) const noexcept {
        return (failedMask & (1u << static_cast<int>(s))) != 0;
    }
};

// Deterministic 24-bit-compare RNG, the detectionHash idiom: no shared state, so the pick is
// independent of evaluation order and identical across workers, platforms and replays.
[[nodiscard]] inline uint32_t subsystemHash(uint32_t targetIdx, uint64_t tick, uint32_t salt) noexcept {
    uint32_t h = targetIdx * 0x9E3779B1u + static_cast<uint32_t>(tick) * 0x85EBCA77u + salt * 0xC2B2AE35u + 0x27D4EB2Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h & 0x00FFFFFFu; // 24-bit, integer compare — no float-order ambiguity
}

// The directional bias for a subsystem given the hit direction in the target's BODY frame
// (x=forward, y=up, z=right). `hitDirBody` is the direction the damage TRAVELLED (a projectile's
// velocity / the blast-to-victim vector); a hit that came from behind is travelling forward, so a
// forward-pointing hitDir means "struck from the rear" → engines. A zero vector = no location, and
// every bias is 1 (pure weight pick), which is the "no location ⇒ weight-table pick" rule.
[[nodiscard]] inline float subsystemDirectionalBias(Subsystem s, const float hitDirBody[3]) noexcept {
    const float fwd = hitDirBody[0]; // >0: struck from the rear (round flying forward)
    const float right = hitDirBody[2];
    if (fwd == 0.f && right == 0.f && hitDirBody[1] == 0.f)
        return 1.f; // no location
    switch (s) {
    case Subsystem::EngineLeft:
        return (fwd > 0.f ? 2.f : 0.6f) * (right < 0.f ? 1.5f : 0.6f); // rear-left
    case Subsystem::EngineRight:
        return (fwd > 0.f ? 2.f : 0.6f) * (right > 0.f ? 1.5f : 0.6f); // rear-right
    case Subsystem::Engine:
        return fwd > 0.f ? 2.f : 0.6f; // centreline engine — rear-struck, no left/right bias (#901)
    case Subsystem::Avionics:
        return fwd < 0.f ? 2.5f : 0.5f; // nose (struck from the front → round flying aft → fwd<0)
    case Subsystem::Fuel:
        return 1.f + 0.5f * (right < 0.f ? -right : right); // wing tanks
    case Subsystem::Controls:
    case Subsystem::Hydraulics:
    case Subsystem::Count:
        break;
    }
    return 1.f; // controls/hydraulics are distributed
}

// Pick which subsystem a hit damages: weight × directional bias over the PRESENT subsystems
// (hp > 0 AND not already failed — a dead subsystem cannot soak more), then a deterministic
// weighted draw from the hash. Returns Subsystem::Count when nothing is eligible.
[[nodiscard]] inline Subsystem pickSubsystem(const SubsystemSet& def, const SubsystemStateSet& state,
                                             const float hitDirBody[3], uint32_t hash24) noexcept {
    float weights[kSubsystemCount];
    float total = 0.f;
    for (int i = 0; i < kSubsystemCount; ++i) {
        const Subsystem s = static_cast<Subsystem>(i);
        const bool eligible = def.parts[i].hp > 0.f && !state.failed(s);
        weights[i] = eligible ? def.parts[i].weight * subsystemDirectionalBias(s, hitDirBody) : 0.f;
        total += weights[i];
    }
    if (total <= 0.f)
        return Subsystem::Count;

    const float r = (static_cast<float>(hash24) / static_cast<float>(0x01000000)) * total;
    float cum = 0.f;
    for (int i = 0; i < kSubsystemCount; ++i) {
        cum += weights[i];
        if (r < cum)
            return static_cast<Subsystem>(i);
    }
    return static_cast<Subsystem>(kSubsystemCount - 1); // float-rounding backstop
}

// Apply `amount` to a subsystem's pool. Returns the bit that NEWLY failed (0 if it survived or was
// already failed), so the caller applies the subsystem's effect exactly once on the failure edge.
[[nodiscard]] inline uint8_t applySubsystemDamage(SubsystemStateSet& state, Subsystem s, float amount) noexcept {
    const int i = static_cast<int>(s);
    if (i < 0 || i >= kSubsystemCount || state.failed(s))
        return 0;
    state.hp[i] -= amount;
    if (state.hp[i] <= 0.f) {
        state.hp[i] = 0.f;
        state.failedMask |= static_cast<uint8_t>(1u << i);
        return static_cast<uint8_t>(1u << i);
    }
    return 0;
}

} // namespace fl
