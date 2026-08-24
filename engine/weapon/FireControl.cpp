// SPDX-License-Identifier: GPL-3.0-or-later
#include "weapon/FireControl.h"

#include "entity/EntityDef.h"
#include "weapon/Loadout.h"

#include <algorithm>

namespace fl {

namespace {

// True when `store` is empty or an explicit "leave this station empty" sentinel.
bool isEmptyStore(const std::string& store) noexcept {
    return store.empty() || store == "~" || store == "-";
}

// Choose the default selection: the first mounted store that is NOT the gun ("select" means the thing
// on the rails; the gun has its own trigger), else the first mounted station, else none. Shared by
// buildLoadout and buildLoadoutOverride. The loop index is size_t (not uint8_t) so the comparison
// against stations.size() is not a narrower-vs-wider one — a >255-station airframe would otherwise
// loop forever; `selected` is uint8_t, so an index past 255 simply stays unselected.
void pickDefaultSelection(LoadoutState& ls, const WeaponRegistry& weapons) {
    for (std::size_t i = 0; i < ls.stations.size() && i <= 255; ++i) {
        const StationState& st = ls.stations[i];
        if (st.weaponIndex == UINT32_MAX)
            continue;
        const WeaponDef* w = weapons.byIndex(st.weaponIndex);
        if (w && w->type != WeaponType::Gun) {
            ls.selected = static_cast<uint8_t>(i);
            return;
        }
    }
    for (std::size_t i = 0; i < ls.stations.size() && i <= 255; ++i) {
        if (ls.stations[i].weaponIndex != UINT32_MAX) {
            ls.selected = static_cast<uint8_t>(i);
            return;
        }
    }
}

// Hang one store on one station: the payload accounting, and the inert-store gate that decides
// whether the station can also FIRE it (#1265).
//
// All three loadout builders — the default, the per-seat subset and the mission override — repeated
// these five lines, and the #862 inert-store rationale was written out twice of the three. A store
// that adds its mass but forgets its drag, or one that becomes fireable in one builder and not
// another, is exactly the divergence one body removes.
void mountStore(LoadoutState& ls, StationState& st, uint32_t idx, const WeaponDef& w) {
    ls.payloadMassKg += w.load.massKg;
    ls.payloadCd0 += w.load.dragFactor;
    // An INERT store — a Fuel drop tank or a Pod (#862) — costs mass/drag but is never a firing
    // station. Inert-ness is the WEAPON's kind, not the station's: the same wet pylon can offer a
    // bomb or a tank, and only the mounted store decides.
    if (!isInertStore(w.type)) {
        st.weaponIndex = idx;
        st.rounds = w.load.rounds;
    }
}

} // namespace

LoadoutState buildLoadout(const EntityDef& def, const WeaponRegistry& weapons) {
    LoadoutState ls;
    ls.stations.reserve(def.hardpoints.size());

    for (const Hardpoint& hp : def.hardpoints) {
        StationState st;
        if (!hp.defaultWeapon.empty()) {
            const uint32_t idx = weapons.indexById(hp.defaultWeapon.c_str());
            if (idx != UINT32_MAX) {
                const WeaponDef* w = weapons.byIndex(idx);
                // Same acceptance rule as fl::defaultPayload: a wrong-kind or unknown store is a
                // skip, not a spawn failure — and it was already Error-logged at payload time.
                if (w)
                    mountStore(ls, st, idx, *w);
            }
        }
        ls.stations.push_back(st);
    }

    // Default selection: the first mounted non-gun store, else any mounted station.
    pickDefaultSelection(ls, weapons);
    return ls;
}

LoadoutState buildSeatLoadout(const EntityDef& def, const WeaponRegistry& weapons, const std::vector<int>& seatSlots) {
    LoadoutState ls;
    for (const Hardpoint& hp : def.hardpoints) {
        if (std::find(seatSlots.begin(), seatSlots.end(), hp.slot) == seatSlots.end())
            continue; // not this seat's station
        StationState st;
        if (!hp.defaultWeapon.empty()) {
            const uint32_t idx = weapons.indexById(hp.defaultWeapon.c_str());
            const WeaponDef* w = (idx != UINT32_MAX) ? weapons.byIndex(idx) : nullptr;
            if (w)
                mountStore(ls, st, idx, *w);
        }
        ls.stations.push_back(st);
    }
    pickDefaultSelection(ls, weapons);
    return ls;
}

LoadoutState buildLoadoutOverride(const EntityDef& def, const WeaponRegistry& weapons,
                                  const std::vector<std::string>& stores, std::vector<std::string>& warnings) {
    LoadoutState ls;
    ls.stations.reserve(def.hardpoints.size());

    for (std::size_t i = 0; i < def.hardpoints.size(); ++i) {
        const Hardpoint& hp = def.hardpoints[i];
        StationState st;

        // Fewer overrides than stations -> keep this station's DEFAULT store (a partial override only
        // touches the stations it names). An empty/"~"/"-" override empties the station deliberately,
        // and an empty default is an empty station.
        const bool named = i < stores.size();
        const std::string& store = named ? stores[i] : hp.defaultWeapon;
        if (isEmptyStore(store)) {
            ls.stations.push_back(st);
            continue;
        }

        const std::string where = "entity '" + def.id + "' station " + std::to_string(hp.slot);
        // A mission may only hang a store the airframe accepts on that station. This check used to
        // be SKIPPED for stations typed fuel/pod (the inert branch ran first), so a mission could
        // hang any tank on any station unchecked; inert-ness is now the mounted weapon's kind, and
        // every store passes the same acceptance gate.
        if (std::find(hp.allowed.begin(), hp.allowed.end(), store) == hp.allowed.end()) {
            warnings.push_back(where + ": store '" + store + "' is not in this station's allowed list; left empty");
            ls.stations.push_back(st);
            continue;
        }
        const uint32_t idx = weapons.indexById(store.c_str());
        const WeaponDef* w = (idx != UINT32_MAX) ? weapons.byIndex(idx) : nullptr;
        if (!w) {
            warnings.push_back(where + ": store '" + store + "' is not a known weapon id; left empty");
            ls.stations.push_back(st);
            continue;
        }
        mountStore(ls, st, idx, *w);
        ls.stations.push_back(st);
    }

    pickDefaultSelection(ls, weapons);
    return ls;
}

void evaluateFire(FireState& fs, const WeaponRegistry& weapons, const WeaponControls& wc, bool weaponsHold,
                  uint64_t tick, uint32_t shooterIdx, std::vector<FireRequest>& out) {
    // The edge detector runs UNCONDITIONALLY, before any gate: a press that arrives during a
    // weapons hold must not be banked and fired the instant the hold lifts.
    const bool releaseEdge = wc.release && !fs.prevRelease;
    fs.prevRelease = wc.release;

    if (fs.loadout.empty())
        return;

    // Absolute station selection (255 = keep). Clamp rather than reject: a client racing a
    // rearm/config change should land on a real station, not have its selection dropped.
    if (wc.station != 255 && !fs.loadout.stations.empty())
        fs.loadout.selected = std::min<uint8_t>(wc.station, static_cast<uint8_t>(fs.loadout.stations.size() - 1));

    if (weaponsHold)
        return; // #610's order, finally with teeth: intent is read, nothing leaves the aircraft

    // Gun trigger: level semantics, rate-limited. Fires the FIRST gun station with rounds — an
    // aircraft with two gun stations (rare) empties them in slot order.
    if (wc.trigger && tick >= fs.nextGunTick) {
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

    // Store release. Single stores (missiles, bombs) are edge-triggered — one press, one store.
    // ROCKETS ripple (#629): level semantics spaced by kRocketRippleTicks while the button is
    // held, because a rocket pod is a volume weapon and "one press, one rocket" would make a
    // 19-round pod a chore. Mass and drag shed PER ROUND (the store's load divided by its
    // magazine), so a half-empty pod costs half as much as a full one.
    StationState* st = fs.loadout.selectedStation();
    const WeaponDef* sw = (st && st->weaponIndex != UINT32_MAX) ? weapons.byIndex(st->weaponIndex) : nullptr;
    const bool rocket = sw && sw->type == WeaponType::Rocket;
    const bool wantsRelease = rocket ? wc.release : releaseEdge;
    if (wantsRelease && tick >= fs.nextReleaseTick) {
        if (st && sw && st->rounds > 0 && sw->type != WeaponType::Gun) {
            fs.nextReleaseTick = tick + (rocket ? kRocketRippleTicks : kReleaseCooldownTicks);
            const float perRound = 1.f / static_cast<float>(std::max<uint16_t>(1u, sw->load.rounds));
            --st->rounds;

            // The store leaves the rails: its mass and drag leave the airframe with it. This
            // is where the live loadout diverges from the per-type default (#812) — and why
            // the own-entity snapshot record now carries the payload (#625).
            fs.loadout.payloadMassKg = std::max(0.f, fs.loadout.payloadMassKg - sw->load.massKg * perRound);
            fs.loadout.payloadCd0 = std::max(0.f, fs.loadout.payloadCd0 - sw->load.dragFactor * perRound);

            FireRequest req;
            req.kind = FireRequest::Kind::Spawn;
            req.shooterIdx = shooterIdx;
            req.weaponIndex = st->weaponIndex;
            req.station = fs.loadout.selected;
            out.push_back(req);
        }
    }
}

} // namespace fl
