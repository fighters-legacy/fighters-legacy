// SPDX-License-Identifier: GPL-3.0-or-later
#include "weapon/FireControl.h"

#include "entity/EntityDef.h"
#include "weapon/Loadout.h"

#include <algorithm>

namespace fl {

LoadoutState buildLoadout(const EntityDef& def, const WeaponRegistry& weapons) {
    LoadoutState ls;
    ls.stations.reserve(def.hardpoints.size());

    for (const Hardpoint& hp : def.hardpoints) {
        StationState st;
        if (!hp.defaultWeapon.empty() && hp.type != HardpointType::Fuel && hp.type != HardpointType::Pod) {
            const uint32_t idx = weapons.indexById(hp.defaultWeapon.c_str());
            if (idx != UINT32_MAX) {
                const WeaponDef* w = weapons.byIndex(idx);
                // Same acceptance rule as fl::defaultPayload: a wrong-kind or unknown store is a
                // skip, not a spawn failure — and it was already Error-logged at payload time.
                if (w) {
                    st.weaponIndex = idx;
                    st.rounds = w->load.rounds;
                    ls.payloadMassKg += w->load.massKg;
                    ls.payloadCd0 += w->load.dragFactor;
                }
            }
        }
        ls.stations.push_back(st);
    }

    // Default selection: the first mounted store that is not the gun — "select" means the thing on
    // the rails; the gun has its own trigger. Fall back to any mounted station (gun included).
    for (uint8_t i = 0; i < ls.stations.size(); ++i) {
        const StationState& st = ls.stations[i];
        if (st.weaponIndex == UINT32_MAX)
            continue;
        const WeaponDef* w = weapons.byIndex(st.weaponIndex);
        if (w && w->type != WeaponType::Gun) {
            ls.selected = i;
            return ls;
        }
    }
    for (uint8_t i = 0; i < ls.stations.size(); ++i) {
        if (ls.stations[i].weaponIndex != UINT32_MAX) {
            ls.selected = i;
            break;
        }
    }
    return ls;
}

void evaluateFire(FireState& fs, const WeaponRegistry& weapons, const ControlInput& in, bool weaponsHold, uint64_t tick,
                  uint32_t shooterIdx, std::vector<FireRequest>& out) {
    // The edge detector runs UNCONDITIONALLY, before any gate: a press that arrives during a
    // weapons hold must not be banked and fired the instant the hold lifts.
    const bool releaseEdge = in.release && !fs.prevRelease;
    fs.prevRelease = in.release;

    if (fs.loadout.empty())
        return;

    // Absolute station selection (255 = keep). Clamp rather than reject: a client racing a
    // rearm/config change should land on a real station, not have its selection dropped.
    if (in.station != 255 && !fs.loadout.stations.empty())
        fs.loadout.selected = std::min<uint8_t>(in.station, static_cast<uint8_t>(fs.loadout.stations.size() - 1));

    if (weaponsHold)
        return; // #610's order, finally with teeth: intent is read, nothing leaves the aircraft

    // Gun trigger: level semantics, rate-limited. Fires the FIRST gun station with rounds — an
    // aircraft with two gun stations (rare) empties them in slot order.
    if (in.trigger && tick >= fs.nextGunTick) {
        for (uint8_t i = 0; i < fs.loadout.stations.size(); ++i) {
            StationState& st = fs.loadout.stations[i];
            if (st.weaponIndex == UINT32_MAX || st.rounds == 0)
                continue;
            const WeaponDef* w = weapons.byIndex(st.weaponIndex);
            if (!w || w->type != WeaponType::Gun)
                continue;

            const float rpm = (w->performance.rateOfFireRpm > 0.f) ? w->performance.rateOfFireRpm : kDefaultGunRpm;
            const uint64_t interval = std::max<uint64_t>(1u, static_cast<uint64_t>(3600.f / rpm)); // 60 Hz ticks
            fs.nextGunTick = tick + interval;

            --st.rounds; // gun mass is the ammunition's; per-round mass loss is noise — not modelled

            FireRequest req;
            req.kind = FireRequest::Kind::Hitscan;
            req.shooterIdx = shooterIdx;
            req.weaponIndex = st.weaponIndex;
            req.station = i;
            out.push_back(req);
            break;
        }
    }

    // Store release: edge semantics, cooldown-spaced.
    if (releaseEdge && tick >= fs.nextReleaseTick) {
        StationState* st = fs.loadout.selectedStation();
        if (st && st->weaponIndex != UINT32_MAX && st->rounds > 0) {
            const WeaponDef* w = weapons.byIndex(st->weaponIndex);
            if (w && w->type != WeaponType::Gun) {
                fs.nextReleaseTick = tick + kReleaseCooldownTicks;
                --st->rounds;

                // The store leaves the rails: its mass and drag leave the airframe with it. This
                // is where the live loadout diverges from the per-type default (#812) — and why
                // the own-entity snapshot record now carries the payload (#625).
                fs.loadout.payloadMassKg = std::max(0.f, fs.loadout.payloadMassKg - w->load.massKg);
                fs.loadout.payloadCd0 = std::max(0.f, fs.loadout.payloadCd0 - w->load.dragFactor);

                FireRequest req;
                req.kind = FireRequest::Kind::Spawn;
                req.shooterIdx = shooterIdx;
                req.weaponIndex = st->weaponIndex;
                req.station = fs.loadout.selected;
                out.push_back(req);
            }
        }
    }
}

} // namespace fl
