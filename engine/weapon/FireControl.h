// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
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
    // ── crew seat (#969) ──
    // Which crew seat fired this request. 0 for the single-seat / pilot path, so the sort key
    // (shooterIdx, seat, station) is identical to the old (shooterIdx, station) for a plain fighter.
    // The crewed weapons pass stamps it; evaluateFire leaves it 0.
    uint8_t seat{0};
    // ── turret launch direction (#970) ──
    // World-space bore the shot leaves along, when the firing station is turret-mounted. Default
    // (hasAimDir == false) = the airframe nose, so a nose-fired shot is bit-identical to before.
    // evaluateFire leaves this default; the caller (the crewed weapons pass, #969) fills it from the
    // seat's turret pose via turretWorldDir. FireRequest's sort key gains seat when crew lands.
    bool hasAimDir{false};
    float aimDir[3]{0.f, 0.f, 0.f};
    // ── launch designation (#1208) ──
    // The target the SHOOTER'S CONTROLLER already picked. Null (the default) = the fire path
    // designates for itself from the shooter's look axis, which is what a player and a nose-pointing
    // aircraft AI want. A controller that aims off-nose fills it via IEntityController::designatedTarget,
    // because re-designating from the airframe nose finds nothing for a launcher whose nose is
    // horizontal — and a seeker weapon launched with no designation flies dumb by documented rule.
    // evaluateFire leaves this default; the weapons pass stamps it.
    EntityId designated{};
};

// The gun default when a weapon def does not say (rate_of_fire_rpm == 0).
inline constexpr float kDefaultGunRpm = 1200.f;
// Store releases are spaced even under a held release input from an AI that never lets go.
inline constexpr uint64_t kReleaseCooldownTicks = 30; // 0.5 s at 60 Hz
// Rockets ripple while the release is HELD (#629): a pod is a volume weapon.
inline constexpr uint64_t kRocketRippleTicks = 6; // ~10 rockets/s at 60 Hz

// The fire slice evaluateFire reads (#969): the trigger/release/selected-station triple. Both a
// pilot's ControlInput and a seat bot's SeatCommand project onto it, so one fire evaluator serves
// the single-seat path and each crew seat's channel.
struct WeaponControls {
    bool trigger{false};
    bool release{false};
    uint8_t station{255}; // absolute; 255 = keep the current selection
};

// Project a full flight ControlInput onto its fire slice.
[[nodiscard]] inline WeaponControls weaponControlsOf(const ControlInput& in) noexcept {
    return WeaponControls{in.trigger, in.release, in.station};
}

// Evaluate one fire channel's intent for this tick (#625). Applies, in order: the wingman
// weapons-hold order (#610 — the flag that "has no teeth until weapons land"; these are the
// teeth), station selection (absolute, clamped; 255 = keep), the store-release EDGE (a held or
// stale-repeated input is one shot), the gun rate limit, ammo. Decrements rounds and the live
// payload (mass + drag leave the airframe with the store) and appends the validated requests.
//
// Deterministic and side-effect-free beyond `fs` — no dice, no world access. Sim-thread only.
// The crewed weapons pass calls this once per Fire seat over that seat's disjoint loadout partition
// (the one-owner invariant makes ammo per-seat, so no station mask is needed); it stamps the seat
// index and turret aim direction onto the requests it appended. The single-seat path calls the
// ControlInput overload with the whole loadout — byte-identical to before.
void evaluateFire(FireState& fs, const WeaponRegistry& weapons, const WeaponControls& wc, bool weaponsHold,
                  uint64_t tick, uint32_t shooterIdx, std::vector<FireRequest>& out);

inline void evaluateFire(FireState& fs, const WeaponRegistry& weapons, const ControlInput& in, bool weaponsHold,
                         uint64_t tick, uint32_t shooterIdx, std::vector<FireRequest>& out) {
    evaluateFire(fs, weapons, weaponControlsOf(in), weaponsHold, tick, shooterIdx, out);
}

} // namespace fl
