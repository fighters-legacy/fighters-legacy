// SPDX-License-Identifier: GPL-3.0-or-later
#include "weapon/WeaponDefParser.h"

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

// ── helpers ──────────────────────────────────────────────────────────────────

[[nodiscard]] std::string req_string(toml::node_view<toml::node> node, const char* field) {
    auto v = node.value<std::string>();
    if (!v)
        throw std::runtime_error(std::string("missing required field: ") + field);
    return std::move(*v);
}

[[nodiscard]] float req_float(toml::node_view<toml::node> node, const char* field) {
    auto v = node.value<double>();
    if (!v)
        throw std::runtime_error(std::string("missing required field: ") + field);
    return static_cast<float>(*v);
}

[[nodiscard]] float opt_float(toml::node_view<toml::node> node, float fallback) {
    auto v = node.value<double>();
    return v ? static_cast<float>(*v) : fallback;
}

[[nodiscard]] bool opt_bool(toml::node_view<toml::node> node, bool fallback) {
    auto v = node.value<bool>();
    return v ? *v : fallback;
}

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
    throw std::runtime_error(std::string("unknown weapon.type: ") + std::string(s) +
                             " (expected missile, bomb, rocket, gun, or pod)");
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
        s.fovDeg = opt_float(node["fov_deg"], 0.f);
        if (s.fovDeg < 0.f || s.fovDeg > 180.f)
            throw std::runtime_error(std::string(section) + ".fov_deg must be in [0, 180]");

        s.acquisitionRangeM = opt_float(node["acquisition_nm"], 0.f) * kMetresPerNauticalMile;
        require_non_negative(s.acquisitionRangeM, "seeker.acquisition_nm");

        s.fireAndForget = opt_bool(node["fire_and_forget"], false);
        s.requiresDesignator = opt_bool(node["requires_designator"], false);
        w.seeker = s;
    }

    // ── [performance] (required) ─────────────────────────────────────────────
    auto perf = tbl["performance"];
    if (!perf || !perf.as_table())
        throw std::runtime_error("weapon def parse error: missing [performance] table");

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

    // ── [warhead] (required) ─────────────────────────────────────────────────
    auto wh = tbl["warhead"];
    if (!wh || !wh.as_table())
        throw std::runtime_error("weapon def parse error: missing [warhead] table");

    w.warhead.blastRadiusM = req_float(wh["blast_radius_ft"], "warhead.blast_radius_ft") * kMetresPerFoot;
    require_non_negative(w.warhead.blastRadiusM, "warhead.blast_radius_ft");
    w.warhead.damage = req_float(wh["damage"], "warhead.damage");
    require_non_negative(w.warhead.damage, "warhead.damage");

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

    return w;
}

} // namespace fl
