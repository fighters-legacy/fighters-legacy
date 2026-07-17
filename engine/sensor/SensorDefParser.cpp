// SPDX-License-Identifier: GPL-3.0-or-later
#include "sensor/SensorDefParser.h"

#include <toml++/toml.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace fl::sensor {

namespace {

// Authored aviation units → SI, same rule as the weapon parser.
constexpr float kMetresPerNauticalMile = 1852.f;

// An omnidirectional sensor has no cone to author; these are what "everywhere" means to the
// detection math, so it needs no special case for the flag.
constexpr float kFullSphereAzHalfAngleDeg = 180.f;
constexpr float kFullSphereElHalfAngleDeg = 90.f;

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

[[nodiscard]] SensorType parse_type(std::string_view s) {
    if (s == "visual")
        return SensorType::Visual;
    if (s == "ir" || s == "infrared")
        return SensorType::Ir;
    if (s == "radar")
        return SensorType::Radar;
    if (s == "laser")
        return SensorType::Laser;
    throw std::runtime_error(std::string("unknown sensor.type: ") + std::string(s) +
                             " (expected radar, ir, visual, or laser)");
}

void require_half_angle(float value, const std::string& field) {
    if (value <= 0.f || value > 180.f)
        throw std::runtime_error(field + " must be in (0, 180]");
}

// One lobe ([search] or [track]). Half-angles are required unless the sensor is omnidirectional, in
// which case there is nothing to point and they describe a full sphere.
[[nodiscard]] SensorLobe parse_lobe(toml::node_view<toml::node> node, const char* section, bool omnidirectional) {
    const std::string prefix = std::string(section) + ".";

    SensorLobe lobe;

    lobe.azHalfAngleDeg = omnidirectional
                              ? opt_float(node["az_half_angle_deg"], kFullSphereAzHalfAngleDeg)
                              : req_float(node["az_half_angle_deg"], (prefix + "az_half_angle_deg").c_str());
    require_half_angle(lobe.azHalfAngleDeg, prefix + "az_half_angle_deg");

    lobe.elHalfAngleDeg = omnidirectional
                              ? opt_float(node["el_half_angle_deg"], kFullSphereElHalfAngleDeg)
                              : req_float(node["el_half_angle_deg"], (prefix + "el_half_angle_deg").c_str());
    require_half_angle(lobe.elHalfAngleDeg, prefix + "el_half_angle_deg");

    lobe.minRangeM = opt_float(node["min_range_nm"], 0.f) * kMetresPerNauticalMile;
    if (lobe.minRangeM < 0.f)
        throw std::runtime_error(prefix + "min_range_nm must be >= 0");

    lobe.maxRangeM = req_float(node["max_range_nm"], (prefix + "max_range_nm").c_str()) * kMetresPerNauticalMile;
    if (lobe.maxRangeM <= lobe.minRangeM)
        throw std::runtime_error(prefix + "max_range_nm must be greater than " + prefix + "min_range_nm");

    lobe.pod = req_float(node["pod"], (prefix + "pod").c_str());
    if (lobe.pod <= 0.f || lobe.pod > 1.f)
        throw std::runtime_error(prefix + "pod must be in (0, 1]");

    return lobe;
}

} // namespace

SensorDef parseSensorDef(std::string_view toml_src) {
    toml::table tbl;
    try {
        tbl = toml::parse(toml_src);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(std::string("sensor def parse error: TOML parse error: ") + e.what());
    }

    SensorDef s;

    // ── [sensor] (required) ──────────────────────────────────────────────────
    auto sen = tbl["sensor"];
    if (!sen || !sen.as_table())
        throw std::runtime_error("sensor def parse error: missing [sensor] table");

    s.id = req_string(sen["id"], "sensor.id");
    s.name = req_string(sen["name"], "sensor.name");
    s.type = parse_type(req_string(sen["type"], "sensor.type"));
    s.omnidirectional = opt_bool(sen["omnidirectional"], false);
    s.emitter = opt_bool(sen["emitter"], false);
    if (auto role = sen["role"].value<std::string>()) {
        if (*role == "aircraft")
            s.role = SensorRole::Aircraft;
        else if (*role == "seeker")
            s.role = SensorRole::Seeker;
        else
            throw std::runtime_error("unknown sensor.role: " + *role + " (expected aircraft or seeker)");
    }

    // ── [search] (required) ──────────────────────────────────────────────────
    auto search = tbl["search"];
    if (!search || !search.as_table())
        throw std::runtime_error("sensor def parse error: missing [search] table");
    s.search = parse_lobe(search, "search", s.omnidirectional);

    // ── [track] (optional) ───────────────────────────────────────────────────
    // Absent = search-only. An eyeball finds an aircraft; it does not hold a lock on one.
    if (auto track = tbl["track"]; track && track.as_table()) {
        s.track = parse_lobe(track, "track", s.omnidirectional);

        s.lockHoldS = opt_float(track["lock_hold_s"], 0.f);
        if (s.lockHoldS < 0.f || s.lockHoldS > 60.f)
            throw std::runtime_error("track.lock_hold_s must be in [0, 60]");
    }

    return s;
}

} // namespace fl::sensor
