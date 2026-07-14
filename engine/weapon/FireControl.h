// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/AeroForces.h"
#include "weapon/StationState.h"

#include <cstdint>
#include <vector>

namespace fl {

// Per-entity fire-control state (#625): the loadout plus the little pieces of memory firing needs —
// the edge detector for store release and the gun's rate limiter. Lives on the entity's
// ControlledEntity (WorldBroadcaster) so its lifecycle is the entity's.
struct FireState {
    LoadoutState loadout;
    bool prevRelease{false};     // store-release edge detector
    bool seekerCue{false};       // #628: the selected seeker sees the designated target right now —
                                 // the pre-launch growl, replicated as the own-record LOCK flag
    uint64_t nextGunTick{0};     // earliest tick the gun may fire again (rate limit)
    uint64_t nextReleaseTick{0}; // store-release cooldown (ripple guard)
};

// One validated shot, ready for the broadcaster to execute. FireControl never touches the world —
// it decrements ammo and emits requests; hitscan resolution, projectile spawning, effects and
// damage are the caller's (serial) job. That split is what keeps this pure and unit-testable.
struct FireRequest {
    enum class Kind : uint8_t {
        Hitscan, // guns: resolve a ray this tick, no entity spawned
        Spawn,   // missiles/rockets/bombs: spawn a projectile entity
    };
    Kind kind{Kind::Hitscan};
    uint32_t shooterIdx{0};
    uint32_t weaponIndex{UINT32_MAX};
    uint8_t station{255};
};

// The gun default when a weapon def does not say (rate_of_fire_rpm == 0).
inline constexpr float kDefaultGunRpm = 1200.f;
// Store releases are spaced even under a held release input from an AI that never lets go.
inline constexpr uint64_t kReleaseCooldownTicks = 30; // 0.5 s at 60 Hz
// Rockets ripple while the release is HELD (#629): a pod is a volume weapon.
inline constexpr uint64_t kRocketRippleTicks = 6; // ~10 rockets/s at 60 Hz

// Evaluate one entity's fire intent for this tick (#625). Applies, in order: the wingman
// weapons-hold order (#610 — the flag that "has no teeth until weapons land"; these are the
// teeth), station selection (absolute, clamped; 255 = keep), the store-release EDGE (a held or
// stale-repeated input is one shot), the gun rate limit, ammo. Decrements rounds and the live
// payload (mass + drag leave the airframe with the store) and appends the validated requests.
//
// Deterministic and side-effect-free beyond `fs` — no dice, no world access. Sim-thread only.
void evaluateFire(FireState& fs, const WeaponRegistry& weapons, const ControlInput& in, bool weaponsHold, uint64_t tick,
                  uint32_t shooterIdx, std::vector<FireRequest>& out);

} // namespace fl
