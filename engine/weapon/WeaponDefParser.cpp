// SPDX-License-Identifier: GPL-3.0-or-later
#include "weapon/WeaponDefParser.h"

#include "config/TomlRead.h"
#include <toml++/toml.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace fl {

namespace {

// ── unit conversions (authored aviation units → SI) ──────────────────────────

constexpr float kMetresPerNauticalMile = 1852.f;
constexpr float kMetresPerFoot = 0.3048f;
constexpr float kMpsPerKnot = 0.514444f;
constexpr float kKgPerPound = 0.45359237f;

void require_non_negative(float value, const char* field) {
    if (value < 0.f)
        throw std::runtime_error(std::string(field) + " must be >= 0");
}

void require_fraction(float value, const char* field) {
    if (value < 0.f || value > 1.f)
        throw std::runtime_error(std::string(field) + " must be in [0, 1]");
}

[[nodiscard]] WeaponType parse_type(std::string_view s) {
    if (s == "missile")
        return WeaponType::Missile;
    if (s == "bomb")
        return WeaponType::Bomb;
    if (s == "rocket")
        return WeaponType::Rocket;
    if (s == "gun")
        return WeaponType::Gun;
    if (s == "pod")
        return WeaponType::Pod;
    if (s == "fuel")
        return WeaponType::Fuel; // #862: drop-tank store — inert, mass/drag only
    throw std::runtime_error(std::string("unknown weapon.type: ") + std::string(s) +
                             " (expected missile, bomb, rocket, gun, pod, or fuel)");
}

[[nodiscard]] WeaponCategory parse_category(std::string_view s) {
    if (s == "air-to-air")
        return WeaponCategory::AirToAir;
    if (s == "air-to-ground")
        return WeaponCategory::AirToGround;
    if (s == "air-to-sea")
        return WeaponCategory::AirToSea;
    if (s == "anti-radiation")
        return WeaponCategory::AntiRadiation;
    throw std::runtime_error(std::string("unknown weapon.category: ") + std::string(s) +
                             " (expected air-to-air, air-to-ground, air-to-sea, or anti-radiation)");
}

[[nodiscard]] SeekerType parse_seeker_type(std::string_view s, const char* field) {
    if (s == "active-radar")
        return SeekerType::ActiveRadar;
    if (s == "semi-active-radar")
        return SeekerType::SemiActiveRadar;
    if (s == "ir" || s == "infrared")
        return SeekerType::Infrared;
    if (s == "laser")
        return SeekerType::Laser;
    if (s == "gps")
        return SeekerType::Gps;
    if (s == "anti-radiation")
        return SeekerType::AntiRadiation;
    if (s == "unguided")
        return SeekerType::Unguided;
    throw std::runtime_error(std::string("unknown ") + field + ".type: " + std::string(s) +
                             " (expected active-radar, semi-active-radar, ir, laser, gps, "
                             "anti-radiation, or unguided)");
}

} // namespace

WeaponDef parseWeaponDef(std::string_view toml_src) {
    toml::table tbl;
    try {
        tbl = toml::parse(toml_src);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(std::string("weapon def parse error: TOML parse error: ") + e.what());
    }

    WeaponDef w;

    // ── [weapon] (required) ──────────────────────────────────────────────────
    auto wpn = tbl["weapon"];
    if (!wpn || !wpn.as_table())
        throw std::runtime_error("weapon def parse error: missing [weapon] table");

    w.id = req_string(wpn["id"], "weapon.id");
    w.name = req_string(wpn["name"], "weapon.name");
    w.type = parse_type(req_string(wpn["type"], "weapon.type"));
    w.category = parse_category(req_string(wpn["category"], "weapon.category"));
    w.mesh = wpn["mesh"].value<std::string>().value_or(std::string{}); // ASSET NAME; empty = builtin

    // ── [seeker] / [guidance] (optional, mutually exclusive) ─────────────────
    // Two authored spellings of one concept: [seeker] is self-guided (the weapon looks), [guidance]
    // is externally guided (someone else looks). They parse into the same struct.
    const bool hasSeeker = tbl["seeker"] && tbl["seeker"].as_table();
    const bool hasGuidance = tbl["guidance"] && tbl["guidance"].as_table();
    if (hasSeeker && hasGuidance)
        throw std::runtime_error("weapon def parse error: [seeker] and [guidance] are mutually exclusive");

    if (hasSeeker || hasGuidance) {
        const char* section = hasSeeker ? "seeker" : "guidance";
        auto node = hasSeeker ? tbl["seeker"] : tbl["guidance"];

        SeekerDef s;
        s.type = parse_seeker_type(req_string(node["type"], hasSeeker ? "seeker.type" : "guidance.type"), section);

        // The seeker head is a SENSOR (2026-07-14 decision record): sensor_id references a sensor
        // def and the missile evaluates it through the same Detection.h math as every observer.
        s.sensorId = node["sensor_id"].value<std::string>().value_or(std::string{});

        // DEPRECATED pre-#583 ad-hoc lobe. Still parsed so existing packs load; the bootstrap and
        // validate-weapon both warn. Authoring both forms is an error, not a precedence puzzle.
        s.fovDeg = opt_float(node["fov_deg"], 0.f);
        if (s.fovDeg < 0.f || s.fovDeg > 180.f)
            throw std::runtime_error(std::string(section) + ".fov_deg must be in [0, 180]");
        s.acquisitionRangeM = opt_float(node["acquisition_nm"], 0.f) * kMetresPerNauticalMile;
        require_non_negative(s.acquisitionRangeM, "seeker.acquisition_nm");
        if (!s.sensorId.empty() && (s.fovDeg > 0.f || s.acquisitionRangeM > 0.f))
            throw std::runtime_error(std::string(section) +
                                     ": sensor_id and the legacy fov_deg/acquisition_nm lobe are "
                                     "mutually exclusive — the sensor def carries the lobe now");

        s.fireAndForget = opt_bool(node["fire_and_forget"], false);
        s.requiresDesignator = opt_bool(node["requires_designator"], false);

        // `pitbull_range_m` is a spelling weapons-sensors.md documented and this parser never read
        // (#1106) — an ARH missile authored per that guide went active straight off the rail, with
        // no error. Name it rather than ignore it: the field exists, it is `pitbull_nm`, and it is
        // authored in nautical miles like every other range on a weapon def.
        if (node["pitbull_range_m"])
            throw std::runtime_error(std::string(section) +
                                     ".pitbull_range_m does not exist — the field is `pitbull_nm`, "
                                     "in nautical miles (range-to-go, like every other weapon range)");
        s.pitbullRangeM = opt_float(node["pitbull_nm"], 0.f) * kMetresPerNauticalMile;
        require_non_negative(s.pitbullRangeM, "seeker.pitbull_nm");
        if (s.pitbullRangeM > 0.f && s.type != SeekerType::ActiveRadar)
            throw std::runtime_error(std::string(section) + ".pitbull_nm only means something on an "
                                                            "active-radar seeker (going active IS the pitbull)");

        s.loftBiasDeg = opt_float(node["loft_bias_deg"], 0.f);
        if (s.loftBiasDeg < 0.f || s.loftBiasDeg > 45.f)
            throw std::runtime_error(std::string(section) + ".loft_bias_deg must be in [0, 45]");
        s.loftRangeM = opt_float(node["loft_range_nm"], 0.f) * kMetresPerNauticalMile;
        require_non_negative(s.loftRangeM, "seeker.loft_range_nm");
        if ((s.loftBiasDeg > 0.f) != (s.loftRangeM > 0.f))
            throw std::runtime_error(std::string(section) +
                                     ": loft_bias_deg and loft_range_nm come as a pair — one says how "
                                     "steep, the other says until when");

        w.seeker = s;
    }

    // An inert store (#862) — a `fuel` drop tank or a `pod` (ECM/targeting/recon) — has no reach, no
    // seeker, and no warhead, only mass and drag. So [performance] and [warhead] are OPTIONAL for it
    // (and default to zero); for every real weapon they stay required.
    const bool isInertStore = w.type == WeaponType::Fuel || w.type == WeaponType::Pod;

    // ── [performance] (required, except for an inert store) ──────────────────
    auto perf = tbl["performance"];
    if (!isInertStore && (!perf || !perf.as_table()))
        throw std::runtime_error("weapon def parse error: missing [performance] table");

    if (isInertStore && (!perf || !perf.as_table())) {
        // No performance section: an inert tank stays all-zero. Skip the range requirement entirely.
    } else {
        // A powered weapon states its own reach (max_range_nm); a dropped one states how far it glides
        // from release (standoff_range_ft). Exactly one of them is the weapon's range.
        const bool hasMaxRange = perf["max_range_nm"].value<double>().has_value();
        const bool hasStandoff = perf["standoff_range_ft"].value<double>().has_value();
        if (!hasMaxRange && !hasStandoff)
            throw std::runtime_error("performance: one of max_range_nm or standoff_range_ft is required");
        if (hasMaxRange && hasStandoff)
            throw std::runtime_error("performance: max_range_nm and standoff_range_ft are mutually exclusive");

        w.performance.maxRangeM =
            hasMaxRange ? req_float(perf["max_range_nm"], "performance.max_range_nm") * kMetresPerNauticalMile
                        : req_float(perf["standoff_range_ft"], "performance.standoff_range_ft") * kMetresPerFoot;
        require_non_negative(w.performance.maxRangeM, "performance range");

        w.performance.minRangeM = opt_float(perf["min_range_nm"], 0.f) * kMetresPerNauticalMile;
        require_non_negative(w.performance.minRangeM, "performance.min_range_nm");
        if (w.performance.minRangeM > w.performance.maxRangeM)
            throw std::runtime_error("performance.min_range_nm must not exceed the weapon's max range");

        w.performance.maxSpeedMps = opt_float(perf["max_speed_kts"], 0.f) * kMpsPerKnot;
        require_non_negative(w.performance.maxSpeedMps, "performance.max_speed_kts");

        w.performance.motorBurnTimeS = opt_float(perf["motor_burn_time_s"], 0.f);
        require_non_negative(w.performance.motorBurnTimeS, "performance.motor_burn_time_s");

        w.performance.maxG = opt_float(perf["max_g"], 0.f);
        require_non_negative(w.performance.maxG, "performance.max_g");

        w.performance.cepM = opt_float(perf["CEP_ft"], 0.f) * kMetresPerFoot;
        require_non_negative(w.performance.cepM, "performance.CEP_ft");

        w.performance.rateOfFireRpm = opt_float(perf["rate_of_fire_rpm"], 0.f);
        require_non_negative(w.performance.rateOfFireRpm, "performance.rate_of_fire_rpm");
    } // end [performance]

    // ── [warhead] (required, except for an inert store) ──────────────────────
    auto wh = tbl["warhead"];
    if (!isInertStore && (!wh || !wh.as_table()))
        throw std::runtime_error("weapon def parse error: missing [warhead] table");

    if (wh && wh.as_table()) {
        w.warhead.blastRadiusM = req_float(wh["blast_radius_ft"], "warhead.blast_radius_ft") * kMetresPerFoot;
        require_non_negative(w.warhead.blastRadiusM, "warhead.blast_radius_ft");
        w.warhead.damage = req_float(wh["damage"], "warhead.damage");
        require_non_negative(w.warhead.damage, "warhead.damage");

        w.warhead.nuclear = opt_bool(wh["nuclear"], false);
        w.warhead.yieldKt = opt_float(wh["yield_kt"], 0.f);
        require_non_negative(w.warhead.yieldKt, "warhead.yield_kt");
        if (w.warhead.nuclear && w.warhead.yieldKt <= 0.f)
            throw std::runtime_error(
                "warhead: nuclear = true requires a yield_kt > 0 — the effect radii scale from it");
        if (!w.warhead.nuclear && w.warhead.yieldKt > 0.f)
            throw std::runtime_error("warhead: yield_kt without nuclear = true — say what you mean");
    }

    // ── [countermeasures] (optional) ─────────────────────────────────────────
    if (auto cm = tbl["countermeasures"]; cm && cm.as_table()) {
        w.countermeasures.chaff = opt_float(cm["chaff_susceptibility"], 0.f);
        require_fraction(w.countermeasures.chaff, "countermeasures.chaff_susceptibility");
        w.countermeasures.flare = opt_float(cm["flare_susceptibility"], 0.f);
        require_fraction(w.countermeasures.flare, "countermeasures.flare_susceptibility");
        w.countermeasures.notch = opt_float(cm["notch_susceptibility"], 0.f);
        require_fraction(w.countermeasures.notch, "countermeasures.notch_susceptibility");
    }

    // ── [load] (required) ────────────────────────────────────────────────────
    auto load = tbl["load"];
    if (!load || !load.as_table())
        throw std::runtime_error("weapon def parse error: missing [load] table");

    w.load.massKg = req_float(load["weight_lb"], "load.weight_lb") * kKgPerPound;
    if (w.load.massKg <= 0.f)
        throw std::runtime_error("load.weight_lb must be > 0");
    w.load.dragFactor = req_float(load["drag_factor"], "load.drag_factor");
    require_non_negative(w.load.dragFactor, "load.drag_factor");

    const float rounds = opt_float(load["rounds"], 0.f);
    if (rounds < 0.f || rounds > 65535.f)
        throw std::runtime_error("load.rounds must be in [0, 65535]");
    w.load.rounds = static_cast<uint16_t>(rounds);
    if (w.load.rounds == 0)
        w.load.rounds = (w.type == WeaponType::Gun) ? 500 : 1; // the engine default

    return w;
}

} // namespace fl
