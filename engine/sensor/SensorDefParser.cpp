// SPDX-License-Identifier: GPL-3.0-or-later
#include "sensor/SensorDefParser.h"

#include "config/TomlRead.h"
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

    // ── ECCM (#529, optional) ────────────────────────────────────────────────
    // Resistance to noise jamming, [0, 1]. Extends the burn-through range against a jamming target.
    //
    // Read from the [sensor] TABLE, which is where every authoring reference puts it and where TOML
    // scoping puts a key written under the `[sensor]` header. This used to read the ROOT table
    // (#1105): a def authored per docs/modding/weapons-sensors.md silently got eccm = 0 — no error,
    // no warning, just a radar with no burn-through advantage against jamming. A misplaced key at
    // the root is now a hard error naming the fix rather than a silent default, because that is the
    // whole failure mode: the value LOOKS authored and does nothing.
    if (tbl["eccm"])
        throw std::runtime_error("sensor def parse error: `eccm` is at the file root; it belongs "
                                 "inside the [sensor] table (move it above the [search] header)");
    s.eccm = opt_float(sen["eccm"], 0.f);
    if (s.eccm < 0.f || s.eccm > 1.f)
        throw std::runtime_error("sensor eccm must be in [0, 1]");

    return s;
}

} // namespace fl::sensor
