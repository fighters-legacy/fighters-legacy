// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/FlightModelParser.h"

#include <toml++/toml.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace fl {

namespace {

// ── helpers ──────────────────────────────────────────────────────────────────

[[nodiscard]] float req_float(toml::node_view<toml::node> node, const char* field) {
    auto v = node.value<double>();
    if (!v)
        throw std::runtime_error(std::string("missing required field: ") + field);
    return static_cast<float>(*v);
}

[[nodiscard]] std::vector<float> req_float_array(toml::node_view<toml::node> node, const char* field) {
    auto* arr = node.as_array();
    if (!arr || arr->empty())
        throw std::runtime_error(std::string("missing or empty required array: ") + field);
    std::vector<float> out;
    out.reserve(arr->size());
    for (auto& el : *arr) {
        auto v = el.value<double>();
        if (!v)
            throw std::runtime_error(std::string("non-numeric value in array: ") + field);
        out.push_back(static_cast<float>(*v));
    }
    return out;
}

[[nodiscard]] Table2D parse_table2d(toml::table& tbl, const char* rows_key, const char* cols_key) {
    Table2D t;
    t.rows = req_float_array(tbl[rows_key], rows_key);
    t.cols = req_float_array(tbl[cols_key], cols_key);
    t.values = req_float_array(tbl["values"], "values");

    std::size_t expected = t.rows.size() * t.cols.size();
    if (t.values.size() != expected) {
        throw std::runtime_error(
            std::string("table dimension mismatch: ") + rows_key + ".size()=" + std::to_string(t.rows.size()) + " x " +
            cols_key + ".size()=" + std::to_string(t.cols.size()) +
            " but values.size()=" + std::to_string(t.values.size()) + " (expected " + std::to_string(expected) + ")");
    }
    return t;
}

[[nodiscard]] AircraftRole parse_role(std::string_view s) {
    if (s == "fighter")
        return AircraftRole::Fighter;
    if (s == "interceptor")
        return AircraftRole::Interceptor;
    if (s == "attacker")
        return AircraftRole::Attacker;
    if (s == "bomber")
        return AircraftRole::Bomber;
    if (s == "maritime_patrol")
        return AircraftRole::MaritimePatrol;
    if (s == "awacs")
        return AircraftRole::Awacs;
    if (s == "ew")
        return AircraftRole::Ew;
    if (s == "recon")
        return AircraftRole::Recon;
    if (s == "tanker")
        return AircraftRole::Tanker;
    if (s == "transport")
        return AircraftRole::Transport;
    if (s == "trainer")
        return AircraftRole::Trainer;
    throw std::runtime_error(std::string("unknown aircraft type: ") + std::string(s));
}

[[nodiscard]] EngineType parse_engine_type(std::string_view s) {
    if (s == "turbojet")
        return EngineType::Turbojet;
    if (s == "turbofan")
        return EngineType::Turbofan;
    if (s == "turboprop")
        return EngineType::Turboprop;
    if (s == "piston")
        return EngineType::Piston;
    throw std::runtime_error(std::string("unknown engine_type: ") + std::string(s));
}

} // namespace

// ── public entry point ────────────────────────────────────────────────────────

FlightModelData parseFlightModel(std::string_view toml_src) {
    toml::table tbl;
    try {
        tbl = toml::parse(toml_src);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(std::string("TOML parse error: ") + e.what());
    }

    FlightModelData d;

    // ── [aircraft] ────────────────────────────────────────────────────────────
    {
        auto ac = tbl["aircraft"];
        if (!ac)
            throw std::runtime_error("missing [aircraft] table");

        auto name = ac["name"].value<std::string>();
        if (!name)
            throw std::runtime_error("missing aircraft.name");
        d.meta.name = std::move(*name);

        auto type_str = ac["type"].value<std::string>();
        if (!type_str)
            throw std::runtime_error("missing aircraft.type");
        d.meta.role = parse_role(*type_str);

        auto et_str = ac["engine_type"].value<std::string>();
        if (!et_str)
            throw std::runtime_error("missing aircraft.engine_type");
        d.meta.engine_type = parse_engine_type(*et_str);

        d.meta.has_fbw = ac["has_fbw"].value<bool>().value_or(false);
        d.meta.cruise_alt_m = static_cast<float>(ac["cruise_alt_m"].value<double>().value_or(10000.0));

        auto mesh = ac["mesh"].value<std::string>();
        if (!mesh)
            throw std::runtime_error("missing aircraft.mesh");
        d.meta.mesh = std::move(*mesh);

        auto cockpit = ac["cockpit"].value<std::string>();
        if (!cockpit)
            throw std::runtime_error("missing aircraft.cockpit");
        d.meta.cockpit = std::move(*cockpit);
    }

    // ── [flight_model] ────────────────────────────────────────────────────────
    {
        auto fm = tbl["flight_model"];
        if (!fm)
            throw std::runtime_error("missing [flight_model] table");

        d.geometry.mass_kg = req_float(fm["mass_kg"], "flight_model.mass_kg");
        d.geometry.wing_area_m2 = req_float(fm["wing_area_m2"], "flight_model.wing_area_m2");
        d.geometry.wingspan_m = req_float(fm["wingspan_m"], "flight_model.wingspan_m");
        d.geometry.mac_m = req_float(fm["mac_m"], "flight_model.mac_m");
        d.geometry.fuel_kg = req_float(fm["fuel_kg"], "flight_model.fuel_kg");
        d.geometry.ixx_kg_m2 = req_float(fm["ixx_kg_m2"], "flight_model.ixx_kg_m2");
        d.geometry.iyy_kg_m2 = req_float(fm["iyy_kg_m2"], "flight_model.iyy_kg_m2");
        d.geometry.izz_kg_m2 = req_float(fm["izz_kg_m2"], "flight_model.izz_kg_m2");
    }

    // ── [aero.cl_table] ───────────────────────────────────────────────────────
    {
        auto cl = tbl["aero"]["cl_table"];
        if (!cl)
            throw std::runtime_error("missing [aero.cl_table] table");
        if (!cl.as_table())
            throw std::runtime_error("[aero.cl_table] must be a table");
        d.cl_table = parse_table2d(*cl.as_table(), "alpha", "mach");
        if (d.cl_table.rows.size() < 4)
            throw std::runtime_error("aero.cl_table: alpha must have at least 4 breakpoints");
        if (d.cl_table.cols.size() < 2)
            throw std::runtime_error("aero.cl_table: mach must have at least 2 breakpoints");
    }

    // ── [aero.drag_polar] ─────────────────────────────────────────────────────
    {
        auto dp = tbl["aero"]["drag_polar"];
        if (!dp)
            throw std::runtime_error("missing [aero.drag_polar] table");

        d.drag_polar.cd0 = req_float(dp["cd0"], "aero.drag_polar.cd0");
        d.drag_polar.k = req_float(dp["k"], "aero.drag_polar.k");
        d.drag_polar.speedbrake_cd = req_float(dp["speedbrake_cd"], "aero.drag_polar.speedbrake_cd");
        d.drag_polar.gear_cd = req_float(dp["gear_cd"], "aero.drag_polar.gear_cd");
    }

    // ── [aero.cd_wave] (optional) ─────────────────────────────────────────────
    if (auto cw = tbl["aero"]["cd_wave"]; cw && cw.as_table()) {
        Table1D wave;
        wave.keys = req_float_array(cw["mach"], "aero.cd_wave.mach");
        wave.values = req_float_array(cw["values"], "aero.cd_wave.values");
        if (wave.keys.size() != wave.values.size())
            throw std::runtime_error("aero.cd_wave: mach and values arrays must have equal length");
        d.cd_wave = std::move(wave);
    }

    // ── [aero.moments] ────────────────────────────────────────────────────────
    {
        auto m = tbl["aero"]["moments"];
        if (!m)
            throw std::runtime_error("missing [aero.moments] table");

        d.moments.cm_alpha = req_float(m["cm_alpha"], "aero.moments.cm_alpha");
        d.moments.cm_q = req_float(m["cm_q"], "aero.moments.cm_q");
        d.moments.cm_de = req_float(m["cm_de"], "aero.moments.cm_de");
        d.moments.cl_beta = req_float(m["cl_beta"], "aero.moments.cl_beta");
        d.moments.cl_p = req_float(m["cl_p"], "aero.moments.cl_p");
        d.moments.cl_da = req_float(m["cl_da"], "aero.moments.cl_da");
        d.moments.cn_beta = req_float(m["cn_beta"], "aero.moments.cn_beta");
        d.moments.cn_r = req_float(m["cn_r"], "aero.moments.cn_r");
        d.moments.cn_dr = req_float(m["cn_dr"], "aero.moments.cn_dr");
    }

    // ── [aero.limits] ─────────────────────────────────────────────────────────
    {
        auto lim = tbl["aero"]["limits"];
        if (!lim)
            throw std::runtime_error("missing [aero.limits] table");

        d.limits.alpha_stall_deg = req_float(lim["alpha_stall_deg"], "aero.limits.alpha_stall_deg");
        d.limits.max_g_structural = req_float(lim["max_g_structural"], "aero.limits.max_g_structural");
        d.limits.min_g_structural = req_float(lim["min_g_structural"], "aero.limits.min_g_structural");
        d.limits.max_mach = req_float(lim["max_mach"], "aero.limits.max_mach");
    }

    // ── [aero.controls] ───────────────────────────────────────────────────────
    {
        auto ctrl = tbl["aero"]["controls"];
        if (!ctrl)
            throw std::runtime_error("missing [aero.controls] table");

        d.controls.max_elevator_deg = req_float(ctrl["max_elevator_deg"], "aero.controls.max_elevator_deg");
        d.controls.max_aileron_deg = req_float(ctrl["max_aileron_deg"], "aero.controls.max_aileron_deg");
        d.controls.max_rudder_deg = req_float(ctrl["max_rudder_deg"], "aero.controls.max_rudder_deg");
    }

    // ── [aero.tvc] (optional) ─────────────────────────────────────────────────
    if (auto tvc = tbl["aero"]["tvc"]; tvc && tvc.as_table()) {
        TvcData tv;
        tv.min_angle_deg = req_float(tvc["min_angle_deg"], "aero.tvc.min_angle_deg");
        tv.max_angle_deg = req_float(tvc["max_angle_deg"], "aero.tvc.max_angle_deg");
        tv.slew_rate_deg_s = req_float(tvc["slew_rate_deg_s"], "aero.tvc.slew_rate_deg_s");
        d.tvc = tv;
    }

    // ── [wing_sweep] (optional) ───────────────────────────────────────────────
    if (auto ws = tbl["wing_sweep"]; ws && ws.as_table()) {
        WingSweepData wsd;
        wsd.ref_sweep_deg = req_float(ws["ref_sweep_deg"], "wing_sweep.ref_sweep_deg");
        wsd.min_deg = req_float(ws["min_deg"], "wing_sweep.min_deg");
        wsd.max_deg = req_float(ws["max_deg"], "wing_sweep.max_deg");
        wsd.slew_rate_deg_s = req_float(ws["slew_rate_deg_s"], "wing_sweep.slew_rate_deg_s");

        if (wsd.ref_sweep_deg < wsd.min_deg || wsd.ref_sweep_deg > wsd.max_deg)
            throw std::runtime_error("wing_sweep.ref_sweep_deg must be within [min_deg, max_deg]");

        auto sched = ws["schedule"];
        if (!sched || !sched.as_table())
            throw std::runtime_error("missing [wing_sweep.schedule] table");
        wsd.schedule.keys = req_float_array(sched["mach"], "wing_sweep.schedule.mach");
        wsd.schedule.values = req_float_array(sched["sweep"], "wing_sweep.schedule.sweep");
        if (wsd.schedule.keys.size() != wsd.schedule.values.size())
            throw std::runtime_error("wing_sweep.schedule: mach and sweep arrays must have equal length");

        auto parse_sweep_config = [&](const char* sub_key) -> WingSweepConfig {
            auto cfg = ws[sub_key];
            if (!cfg || !cfg.as_table())
                throw std::runtime_error(std::string("missing [wing_sweep.") + sub_key + "] table");
            WingSweepConfig c;
            c.cl_scale = req_float(cfg["cl_scale"], (std::string("wing_sweep.") + sub_key + ".cl_scale").c_str());
            c.k_scale = req_float(cfg["k_scale"], (std::string("wing_sweep.") + sub_key + ".k_scale").c_str());
            c.cd0_delta = req_float(cfg["cd0_delta"], (std::string("wing_sweep.") + sub_key + ".cd0_delta").c_str());
            return c;
        };

        wsd.spread = parse_sweep_config("spread");
        wsd.swept = parse_sweep_config("swept");
        d.wing_sweep = std::move(wsd);
    }

    // ── [prop] (optional) ─────────────────────────────────────────────────────
    if (auto p = tbl["prop"]; p && p.as_table()) {
        PropData pd;
        auto rot = p["rotation"].value<std::string>();
        if (!rot)
            throw std::runtime_error("missing prop.rotation");
        if (*rot == "cw")
            pd.rotation = PropRotation::CW;
        else if (*rot == "ccw")
            pd.rotation = PropRotation::CCW;
        else if (*rot == "contra")
            pd.rotation = PropRotation::Contra;
        else
            throw std::runtime_error(std::string("unknown prop.rotation: ") + *rot);

        pd.torque_factor = req_float(p["torque_factor"], "prop.torque_factor");
        pd.gyro_factor = req_float(p["gyro_factor"], "prop.gyro_factor");
        d.prop = pd;
    }

    // ── [engine] ──────────────────────────────────────────────────────────────
    {
        auto eng = tbl["engine"];
        if (!eng)
            throw std::runtime_error("missing [engine] table");

        d.engine.type = d.meta.engine_type;
        d.engine.fuel_flow_idle_kg_s = req_float(eng["fuel_flow_idle_kg_s"], "engine.fuel_flow_idle_kg_s");
        d.engine.fuel_flow_mil_kg_s = req_float(eng["fuel_flow_mil_kg_s"], "engine.fuel_flow_mil_kg_s");
        d.engine.fuel_flow_ab_kg_s = req_float(eng["fuel_flow_ab_kg_s"], "engine.fuel_flow_ab_kg_s");
        d.engine.spool_time_s = req_float(eng["spool_time_s"], "engine.spool_time_s");

        auto mil = tbl["engine"]["mil_thrust"];
        if (!mil || !mil.as_table())
            throw std::runtime_error("missing [engine.mil_thrust] table");
        d.engine.mil_thrust = parse_table2d(*mil.as_table(), "mach", "alt_km");
        if (d.engine.mil_thrust.rows.size() < 2)
            throw std::runtime_error("engine.mil_thrust: mach must have at least 2 breakpoints");
        if (d.engine.mil_thrust.cols.size() < 2)
            throw std::runtime_error("engine.mil_thrust: alt_km must have at least 2 breakpoints");

        if (auto ab = tbl["engine"]["ab_thrust"]; ab && ab.as_table()) {
            auto abt = parse_table2d(*ab.as_table(), "mach", "alt_km");
            if (abt.rows.size() < 2)
                throw std::runtime_error("engine.ab_thrust: mach must have at least 2 breakpoints");
            if (abt.cols.size() < 2)
                throw std::runtime_error("engine.ab_thrust: alt_km must have at least 2 breakpoints");
            d.engine.ab_thrust = std::move(abt);
        }
    }

    // ── [carrier] (optional) ──────────────────────────────────────────────────
    if (auto c = tbl["carrier"]; c && c.as_table()) {
        CarrierData cd;
        cd.approach_m_s = req_float(c["approach_m_s"], "carrier.approach_m_s");
        cd.approach_aoa_deg = req_float(c["approach_aoa_deg"], "carrier.approach_aoa_deg");
        cd.cat_min_m_s = req_float(c["cat_min_m_s"], "carrier.cat_min_m_s");
        cd.hook_length_m = req_float(c["hook_length_m"], "carrier.hook_length_m");
        d.carrier = cd;
    }

    // ── [refueling] (optional) ────────────────────────────────────────────────
    if (auto r = tbl["refueling"]; r && r.as_table()) {
        RefuelingData rd;
        auto type_str = r["type"].value<std::string>();
        if (!type_str)
            throw std::runtime_error("missing refueling.type");
        if (*type_str == "boom")
            rd.boom = true;
        else if (*type_str == "drogue")
            rd.boom = false;
        else
            throw std::runtime_error(std::string("unknown refueling.type: ") + *type_str);
        rd.max_rate_kg_s = req_float(r["max_rate_kg_s"], "refueling.max_rate_kg_s");
        d.refueling = rd;
    }

    // ── [tanker] (optional) ───────────────────────────────────────────────────
    if (auto t = tbl["tanker"]; t && t.as_table()) {
        TankerData td;
        auto type_str = t["type"].value<std::string>();
        if (!type_str)
            throw std::runtime_error("missing tanker.type");
        if (*type_str == "boom") {
            td.boom = true;
            td.drogue = false;
        } else if (*type_str == "drogue") {
            td.boom = false;
            td.drogue = true;
        } else if (*type_str == "both") {
            td.boom = true;
            td.drogue = true;
        } else {
            throw std::runtime_error(std::string("unknown tanker.type: ") + *type_str);
        }
        td.stations = static_cast<int>(t["stations"].value<int64_t>().value_or(1));
        td.max_rate_kg_s = req_float(t["max_rate_kg_s"], "tanker.max_rate_kg_s");
        td.offload_reserve = req_float(t["offload_reserve"], "tanker.offload_reserve");
        d.tanker = td;
    }

    return d;
}

} // namespace fl
