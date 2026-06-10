// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/FlightModelData.h"
#include "math/Table2D.h"

#include <memory>

namespace fl {

// Compiled-in flight model for the zero-content-pack sandbox.
// Models a UFO-like craft: extremely agile, very low minimum flight speed,
// no stall, near-infinite fuel, T/W ≈ 4. Follows the same FlightIntegrator
// physics as real aircraft — only the parameters differ.
struct BuiltinFlightModel {
    static std::shared_ptr<const FlightModelData> get() {
        static std::shared_ptr<const FlightModelData> kInstance = [] {
            auto d = std::make_shared<FlightModelData>();

            d->meta.name = "builtin:ufo";
            d->meta.role = AircraftRole::Fighter;
            d->meta.engine_type = EngineType::Turbofan;

            // 800 kg, huge wing (S=30 m²) → level flight at ~17 m/s. T/W ≈ 4.
            d->geometry.mass_kg = 800.f;
            d->geometry.wing_area_m2 = 30.f;
            d->geometry.wingspan_m = 8.f;
            d->geometry.mac_m = 3.75f;
            d->geometry.fuel_kg = 200.f; // 200 kg at 0.001 kg/s ≈ 55 h burn — effectively infinite
            d->geometry.ixx_kg_m2 = 4000.f;
            d->geometry.iyy_kg_m2 = 8000.f;
            d->geometry.izz_kg_m2 = 8000.f;

            // CL table: 7 alpha × 2 Mach — speed-independent, no stall.
            // alpha_stall_deg = 90 so no CL cliff; table clamped at edges.
            d->cl_table.rows = {-10.f, 0.f, 10.f, 20.f, 30.f, 45.f, 90.f};
            d->cl_table.cols = {0.f, 2.f};
            d->cl_table.values = {
                -0.3f, -0.3f, // alpha = -10°
                0.2f,  0.2f,  // alpha =   0°
                1.0f,  1.0f,  // alpha =  10°
                1.5f,  1.5f,  // alpha =  20°
                1.5f,  1.5f,  // alpha =  30°
                1.2f,  1.2f,  // alpha =  45°
                0.5f,  0.5f,  // alpha =  90°
            };

            d->drag_polar.cd0 = 0.008f;
            d->drag_polar.k = 0.02f;
            d->drag_polar.speedbrake_cd = 0.f;
            d->drag_polar.gear_cd = 0.f;

            // Pitch damping and elevator authority tuned for stable semi-implicit Euler
            // integration at 60 Hz. cm_q=-8 keeps the pitch-damping stiffness ratio well
            // below the stability threshold; cm_de=-0.5 gives ~90°/s equilibrium pitch
            // rate at 150 m/s — agile but not snap-divergent.
            d->moments.cm_alpha = 0.f;
            d->moments.cm_q = -8.f;
            d->moments.cm_de = -0.5f;
            d->moments.cl_beta = 0.f;
            d->moments.cl_p = -2.f;
            d->moments.cl_da = 0.5f;
            d->moments.cn_beta = 0.f;
            d->moments.cn_r = -1.f;
            d->moments.cn_dr = -0.3f;

            d->limits.alpha_stall_deg = 90.f;
            d->limits.max_g_structural = 99.f;
            d->limits.min_g_structural = -99.f;
            d->limits.max_mach = 3.f;

            d->controls.max_elevator_deg = 45.f;
            d->controls.max_aileron_deg = 45.f;
            d->controls.max_rudder_deg = 45.f;

            // Thrust: 30 kN sea level → T/W = 30000 / (800 × 9.8) ≈ 3.8.
            d->engine.type = EngineType::Turbofan;
            d->engine.mil_thrust.rows = {0.f, 0.5f, 2.f}; // Mach
            d->engine.mil_thrust.cols = {0.f, 12.f};      // alt_km
            d->engine.mil_thrust.values = {
                30.f, 15.f, // Mach 0.0
                32.f, 17.f, // Mach 0.5
                25.f, 12.f, // Mach 2.0
            };
            d->engine.fuel_flow_idle_kg_s = 0.f;
            d->engine.fuel_flow_mil_kg_s = 0.001f; // negligible burn
            d->engine.fuel_flow_ab_kg_s = 0.001f;
            d->engine.spool_time_s = 0.1f; // near-instant response

            return d;
        }();
        return kInstance;
    }
};

} // namespace fl
