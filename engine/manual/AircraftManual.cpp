// SPDX-License-Identifier: GPL-3.0-or-later
#include "manual/AircraftManual.h"

#include "entity/EntityDef.h"
#include "flight/FlightModelData.h"
#include "flight/Trim.h"
#include "sensor/SensorDef.h"
#include "weapon/WeaponDef.h"
#include "weapon/WeaponRegistry.h"

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

namespace fl {
namespace {

std::string fmt(const char* spec, float v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), spec, static_cast<double>(v));
    return buf;
}

// Aviation units, because that is what a pilot's reference card is written in and what the source
// documents use. The sim is SI internally; the conversion lives here, at the display boundary.
constexpr float kMpsToKt = 1.94384f;
constexpr float kMToFt = 3.28084f;
constexpr float kKgToLb = 2.20462f;

std::string speedStr(float mps) {
    return fmt("%.0f kt", mps * kMpsToKt) + fmt(" (%.0f m/s)", mps);
}

const char* weaponTypeName(WeaponType t) {
    switch (t) {
    case WeaponType::Missile:
        return "missile";
    case WeaponType::Bomb:
        return "bomb";
    case WeaponType::Rocket:
        return "rocket";
    case WeaponType::Gun:
        return "gun";
    case WeaponType::Fuel:
        return "fuel";
    case WeaponType::Pod:
        return "pod";
    }
    return "?";
}

// A station's kind is whatever its allowed stores are: one kind -> that name, several -> "multi-role".
// Stations have no type of their own; the allowed list is the whole compatibility contract.
std::string stationKind(const Hardpoint& hp, const WeaponRegistry* weapons) {
    if (!weapons)
        return {};
    std::string kind;
    bool mixed = false;
    for (const std::string& id : hp.allowed) {
        const WeaponDef* w = weapons->findById(id);
        if (!w)
            continue;
        const char* name = weaponTypeName(w->type);
        if (kind.empty())
            kind = name;
        else if (kind != name)
            mixed = true;
    }
    if (kind.empty())
        return {};
    return mixed ? "multi-role" : kind;
}

const char* sensorTypeName(sensor::SensorType t) {
    switch (t) {
    case sensor::SensorType::Visual:
        return "visual";
    case sensor::SensorType::Ir:
        return "infrared";
    case sensor::SensorType::Radar:
        return "radar";
    case sensor::SensorType::Laser:
        return "laser";
    }
    return "?";
}

// The conditions a fighter's flight manual actually tabulates.
struct NamedCondition {
    const char* label;
    float altitude_m;
};

constexpr NamedCondition kConditions[] = {
    {"sea level", 0.f},
    {"15 000 ft", 4572.f},
    {"36 000 ft", 10973.f},
};

void addPerformance(AircraftManual& out, const ManualSources& src, float mass, const char* weightLabel) {
    ManualSection sec;
    sec.title =
        std::string("Performance — ") + weightLabel + fmt(" (%.0f kg", mass) + fmt(" / %.0f lb)", mass * kKgToLb);

    for (const auto& cond : kConditions) {
        TrimPoint pt;
        pt.altitude_m = cond.altitude_m;
        pt.mass_kg = mass;

        // The SAME fl::trim() the CI gate calls (#817). If someone retunes the drag polar, this
        // number moves with it -- which is the whole point, and is why the manual cannot drift.
        const TrimResult r = trim(*src.model, pt, src.payload);

        if (!r.converged) {
            // Honest: the aircraft cannot hold level flight here, so it has no performance here.
            sec.rows.push_back({std::string(cond.label), "cannot sustain level flight"});
            continue;
        }

        sec.rows.push_back({std::string(cond.label) + " — stall speed (1 g)", speedStr(r.stall_speed_1g_mps)});
        sec.rows.push_back({std::string(cond.label) + " — min level speed", speedStr(r.min_level_speed_mps)});
        sec.rows.push_back({std::string(cond.label) + " — max level speed", fmt("Mach %.2f", r.max_level_mach)});
        sec.rows.push_back({std::string(cond.label) + " — rate of climb",
                            fmt("%.0f m/s MIL", r.roc_mps_mil) + fmt(" / %.0f m/s AB", r.roc_mps_ab)});
        sec.rows.push_back({std::string(cond.label) + " — sustained turn",
                            fmt("%.1f deg/s", r.sustained_turn_deg_s) + fmt(" at %.1f g", r.sustained_g)});
        sec.rows.push_back({std::string(cond.label) + " — instantaneous turn",
                            fmt("%.1f deg/s", r.instant_turn_deg_s) + fmt(" at %.1f g", r.instant_g)});
        sec.rows.push_back({std::string(cond.label) + " — corner speed", speedStr(r.corner_speed_mps)});
        sec.rows.push_back(
            {std::string(cond.label) + " — specific range", fmt("%.0f m/kg", r.specific_range_m_per_kg)});
    }

    out.sections.push_back(std::move(sec));
}

} // namespace

AircraftManual buildAircraftManual(const ManualSources& src) {
    AircraftManual out;
    if (!src.entity || !src.model)
        return out;

    const EntityDef& def = *src.entity;
    const FlightModelData& fm = *src.model;

    out.title = def.name.empty() ? fm.meta.name : def.name;

    // ── Airframe ─────────────────────────────────────────────────────────────
    {
        ManualSection sec;
        sec.title = "Airframe";
        sec.rows.push_back(
            {"empty mass", fmt("%.0f kg", fm.geometry.mass_kg) + fmt(" (%.0f lb)", fm.geometry.mass_kg * kKgToLb)});
        sec.rows.push_back({"internal fuel", fmt("%.0f kg", fm.geometry.fuel_kg)});
        sec.rows.push_back({"wing area", fmt("%.2f m2", fm.geometry.wing_area_m2)});
        sec.rows.push_back({"wingspan", fmt("%.2f m", fm.geometry.wingspan_m)});
        if (fm.geometry.wing_area_m2 > 0.f) {
            const float wingLoading = (fm.geometry.mass_kg + fm.geometry.fuel_kg) / fm.geometry.wing_area_m2;
            sec.rows.push_back({"wing loading (full fuel)", fmt("%.0f kg/m2", wingLoading)});
        }
        out.sections.push_back(std::move(sec));
    }

    // ── Limits: straight off [aero.limits], the fields #816 made load-bearing. ───────────────────
    {
        ManualSection sec;
        sec.title = "Limits";
        sec.rows.push_back({"structural limit",
                            fmt("+%.2f g", fm.limits.max_g_structural) + fmt(" / %.2f g", fm.limits.min_g_structural)});
        sec.rows.push_back({"never exceed", fmt("Mach %.2f", fm.limits.max_mach)});
        sec.rows.push_back({"stall AoA", fmt("%.1f deg", fm.limits.alpha_stall_deg)});
        sec.rows.push_back({"G-limiter", fm.meta.has_fbw ? "fly-by-wire — the aircraft will not exceed its limit"
                                                         : "NONE — the airframe can be overstressed, and will break"});
        sec.rows.push_back({"service ceiling (cruise)",
                            fmt("%.0f m", fm.meta.cruise_alt_m) + fmt(" (%.0f ft)", fm.meta.cruise_alt_m * kMToFt)});
        out.sections.push_back(std::move(sec));
    }

    // ── Performance, clean and combat. ───────────────────────────────────────
    const float cleanMass = fm.geometry.mass_kg + fm.geometry.fuel_kg;
    addPerformance(out, src, cleanMass, "clean");
    if (src.payload.extra_mass_kg > 0.f || src.payload.extra_cd0 > 0.f) {
        // The combat pass carries the default loadout, so the numbers are what the aircraft will
        // actually do when you take off in it -- not what it would do empty.
        addPerformance(out, src, cleanMass, "with default loadout");
    }

    // ── Loadout: from the entity's stations and the weapon registry (#812). ──────────────────────
    if (!def.hardpoints.empty()) {
        ManualSection sec;
        sec.title = "Stations";
        for (const Hardpoint& hp : def.hardpoints) {
            std::ostringstream label;
            label << "station " << hp.slot;
            if (const std::string kind = stationKind(hp, src.weapons); !kind.empty())
                label << " (" << kind << ")";

            std::ostringstream value;
            if (hp.defaultWeapon.empty()) {
                value << "empty";
            } else {
                const WeaponDef* w = src.weapons ? src.weapons->findById(hp.defaultWeapon) : nullptr;
                if (w) {
                    value << w->name;
                    if (w->load.massKg > 0.f)
                        value << " — " << fmt("%.0f kg", w->load.massKg);
                } else {
                    // Say so. A station whose store does not resolve is exactly what the pilot needs
                    // to know, and it is the same condition defaultPayload() logs at Error.
                    value << hp.defaultWeapon << " (unknown store)";
                }
            }
            sec.rows.push_back({label.str(), value.str()});
        }

        if (src.payload.extra_mass_kg > 0.f)
            sec.rows.push_back({"default loadout", fmt("%.0f kg of stores", src.payload.extra_mass_kg) +
                                                       fmt(", +%.4f cd0", src.payload.extra_cd0)});
        out.sections.push_back(std::move(sec));
    }

    // ── Crew: from the entity's authored seats + turret mounts (#966/#977). Generated, like every
    // other section — the role names and turret arcs come straight from the def, the capabilities from
    // the mask. A single-seat aircraft declares no [[crew]] and gets no page (it is an implicit pilot).
    if (!def.crew.empty()) {
        ManualSection sec;
        sec.title = "Crew";
        auto turretById = [&](const std::string& id) -> const TurretDef* {
            for (const TurretDef& t : def.turrets)
                if (t.id == id)
                    return &t;
            return nullptr;
        };
        for (const SeatDef& seat : def.crew) {
            std::ostringstream value;
            bool first = true;
            for (CrewCapability cap : allCrewCapabilities()) {
                if (!hasCapability(seat.capabilities, cap))
                    continue;
                if (!first)
                    value << ", ";
                value << crewCapabilityName(cap);
                first = false;
            }
            if (!seat.turret.empty()) {
                if (const TurretDef* t = turretById(seat.turret))
                    value << " — turret \"" << seat.turret << "\" (" << fmt("%.0f", t->azMinDeg) << " to "
                          << fmt("%.0f", t->azMaxDeg) << " deg az, " << fmt("%.0f", t->elMinDeg) << " to "
                          << fmt("%.0f", t->elMaxDeg) << " deg el)";
            }
            sec.rows.push_back({seat.role, value.str()});
        }
        out.sections.push_back(std::move(sec));
    }

    // ── Sensors: from the entity's sensor ids, resolved by the caller (#810). ────────────────────
    if (!src.sensors.empty()) {
        ManualSection sec;
        sec.title = "Sensors";
        for (const sensor::SensorDef* s : src.sensors) {
            if (!s)
                continue;
            std::ostringstream value;
            value << sensorTypeName(s->type);
            if (s->omnidirectional) {
                value << ", all-aspect";
            } else {
                value << ", " << fmt("%.0f", s->search.azHalfAngleDeg) << " deg azimuth";
            }
            value << ", " << fmt("%.0f nm", s->search.maxRangeM / 1852.f) << " search";
            if (s->track)
                value << ", " << fmt("%.0f nm", s->track->maxRangeM / 1852.f) << " track";
            if (s->emitter)
                value << " (emits — it can be detected)";
            sec.rows.push_back({s->name.empty() ? s->id : s->name, value.str()});
        }
        out.sections.push_back(std::move(sec));
    }

    // ── The pack's own prose: the only hand-written part, and it contains no numbers. ────────────
    if (!src.prose.empty()) {
        std::istringstream in(src.prose);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            out.prose.push_back(line);
        }
    }

    return out;
}

} // namespace fl
