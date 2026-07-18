// SPDX-License-Identifier: GPL-3.0-or-later
#include "entity/CrewDef.h"

#include <algorithm>
#include <string>
#include <unordered_map>

namespace fl {

const std::vector<CrewCapability>& allCrewCapabilities() {
    static const std::vector<CrewCapability> kCaps{CrewCapability::Fly, CrewCapability::Fire, CrewCapability::Radar,
                                                   CrewCapability::Countermeasures, CrewCapability::Command};
    return kCaps;
}

std::string_view crewCapabilityName(CrewCapability cap) noexcept {
    switch (cap) {
    case CrewCapability::Fly:
        return "fly";
    case CrewCapability::Fire:
        return "fire";
    case CrewCapability::Radar:
        return "radar";
    case CrewCapability::Countermeasures:
        return "countermeasures";
    case CrewCapability::Command:
        return "command";
    case CrewCapability::None:
        return "none";
    }
    return "none";
}

std::optional<CrewCapability> parseCrewCapability(std::string_view token) noexcept {
    if (token == "fly")
        return CrewCapability::Fly;
    if (token == "fire")
        return CrewCapability::Fire;
    if (token == "radar")
        return CrewCapability::Radar;
    if (token == "countermeasures")
        return CrewCapability::Countermeasures;
    if (token == "command")
        return CrewCapability::Command;
    return std::nullopt;
}

namespace {

// The slots a seat is responsible for firing = its direct stations plus, if it aims a turret,
// that turret's mounted stations.
[[nodiscard]] std::vector<int> firedSlots(const SeatDef& seat, const std::vector<TurretDef>& turrets) {
    std::vector<int> slots = seat.stations;
    if (!seat.turret.empty()) {
        for (const TurretDef& t : turrets) {
            if (t.id == seat.turret) {
                slots.insert(slots.end(), t.stations.begin(), t.stations.end());
                break;
            }
        }
    }
    return slots;
}

} // namespace

std::string validateCrewPartition(const std::vector<SeatDef>& crew, const std::vector<TurretDef>& turrets,
                                  const std::vector<int>& hardpointSlots) {
    if (crew.empty())
        return {}; // implicit single pilot — nothing to partition

    auto slotExists = [&](int slot) {
        return std::find(hardpointSlots.begin(), hardpointSlots.end(), slot) != hardpointSlots.end();
    };

    // Turret defs: unique ids, resolvable stations, sane limits.
    for (size_t i = 0; i < turrets.size(); ++i) {
        const TurretDef& t = turrets[i];
        if (t.id.empty())
            return "turret " + std::to_string(i) + " has an empty id";
        for (size_t j = 0; j < i; ++j) {
            if (turrets[j].id == t.id)
                return "duplicate turret id \"" + t.id + "\"";
        }
        if (t.slewRateDegS <= 0.f)
            return "turret \"" + t.id + "\" slew_rate_deg_s must be > 0";
        if (t.azMinDeg > t.azMaxDeg)
            return "turret \"" + t.id + "\" az_min_deg must be <= az_max_deg";
        if (t.elMinDeg > t.elMaxDeg)
            return "turret \"" + t.id + "\" el_min_deg must be <= el_max_deg";
        for (int slot : t.stations) {
            if (!slotExists(slot))
                return "turret \"" + t.id + "\" mounts unknown hardpoint slot " + std::to_string(slot);
        }
    }

    int flySeats = 0;
    int radarSeats = 0;
    int cmSeats = 0;
    int commandSeats = 0;
    std::unordered_map<int, int> slotOwner;           // hardpoint slot -> owning seat index
    std::unordered_map<std::string, int> turretOwner; // turret id -> aiming seat index

    for (size_t s = 0; s < crew.size(); ++s) {
        const SeatDef& seat = crew[s];
        const std::string where = "crew seat " + std::to_string(s) + " (\"" + seat.role + "\")";

        if (seat.role.empty())
            return "crew seat " + std::to_string(s) + " has an empty role";
        if (seat.capabilities == 0u)
            return where + " declares no capabilities";

        if (hasCapability(seat.capabilities, CrewCapability::Fly))
            ++flySeats;
        if (hasCapability(seat.capabilities, CrewCapability::Radar))
            ++radarSeats;
        if (hasCapability(seat.capabilities, CrewCapability::Countermeasures))
            ++cmSeats;
        if (hasCapability(seat.capabilities, CrewCapability::Command))
            ++commandSeats;

        const bool bindsGuns = !seat.stations.empty() || !seat.turret.empty();
        if (bindsGuns && !hasCapability(seat.capabilities, CrewCapability::Fire))
            return where + " binds stations/turret but lacks the Fire capability";

        // Turret reference resolves and is claimed by at most one seat.
        if (!seat.turret.empty()) {
            const bool found =
                std::any_of(turrets.begin(), turrets.end(), [&](const TurretDef& t) { return t.id == seat.turret; });
            if (!found)
                return where + " references unknown turret \"" + seat.turret + "\"";
            auto [it, inserted] = turretOwner.emplace(seat.turret, static_cast<int>(s));
            if (!inserted)
                return "turret \"" + seat.turret + "\" is aimed by more than one seat (seats " +
                       std::to_string(it->second) + " and " + std::to_string(s) + ")";
        }

        // Each fired slot resolves and is owned by at most one seat.
        for (int slot : firedSlots(seat, turrets)) {
            if (!slotExists(slot))
                return where + " fires unknown hardpoint slot " + std::to_string(slot);
            auto [it, inserted] = slotOwner.emplace(slot, static_cast<int>(s));
            if (!inserted)
                return "hardpoint slot " + std::to_string(slot) + " is fired by more than one seat (seats " +
                       std::to_string(it->second) + " and " + std::to_string(s) + ")";
        }

        if (hasCapability(seat.capabilities, CrewCapability::Fire) && firedSlots(seat, turrets).empty())
            return where + " has the Fire capability but fires no station (bind stations or a turret)";

        // A bot seat with no explicit spec is legal — the runtime picks an engine-default controller
        // for the seat's dominant capability — so botSpec is not required here.
        if (seat.defaultSkill < 0.f || seat.defaultSkill > 1.f)
            return where + " default skill must be in [0, 1]";
    }

    if (flySeats != 1)
        return "a crewed aircraft must have exactly one Fly seat (found " + std::to_string(flySeats) + ")";
    if (radarSeats > 1)
        return "the Radar capability is on more than one seat (found " + std::to_string(radarSeats) + ")";
    if (cmSeats > 1)
        return "the Countermeasures capability is on more than one seat (found " + std::to_string(cmSeats) + ")";
    if (commandSeats > 1)
        return "the Command capability is on more than one seat (found " + std::to_string(commandSeats) + ")";

    return {};
}

} // namespace fl
