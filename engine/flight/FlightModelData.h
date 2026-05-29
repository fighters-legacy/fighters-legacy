// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "math/Table1D.h"
#include "math/Table2D.h"

#include <optional>
#include <string>

namespace fl {

enum class EngineType { Turbojet, Turbofan, Turboprop, Piston };

enum class AircraftRole {
    Fighter,
    Interceptor,
    Attacker,
    Bomber,
    MaritimePatrol,
    Awacs,
    Ew,
    Recon,
    Tanker,
    Transport,
    Trainer
};

enum class PropRotation { CW, CCW, Contra };

struct AircraftMeta {
    std::string name;
    AircraftRole role{AircraftRole::Fighter};
    EngineType engine_type{EngineType::Turbofan};
    bool has_fbw{false};
    float cruise_alt_m{10000.f};
    std::string mesh;
    std::string cockpit;
};

struct FlightModelGeometry {
    float mass_kg{10000.f};
    float wing_area_m2{35.f};
    float wingspan_m{10.f};
    float mac_m{3.5f};
    float fuel_kg{4000.f};
    float ixx_kg_m2{10000.f};
    float iyy_kg_m2{70000.f};
    float izz_kg_m2{78000.f};
};

struct AeroDragPolar {
    float cd0{0.018f};
    float k{0.14f};
    float speedbrake_cd{0.07f};
    float gear_cd{0.03f};
};

struct AeroMoments {
    // Pitch (reference length: mac_m)
    float cm_alpha{-0.7f};
    float cm_q{-10.f};
    float cm_de{-1.f};
    // Roll (reference length: wingspan_m)
    float cl_beta{-0.08f};
    float cl_p{-0.40f};
    float cl_da{0.07f};
    // Yaw (reference length: wingspan_m)
    float cn_beta{0.10f};
    float cn_r{-0.12f};
    float cn_dr{-0.05f};
};

struct AeroLimits {
    float alpha_stall_deg{18.f};
    float max_g_structural{8.f};
    float min_g_structural{-3.f};
    float max_mach{1.6f};
};

struct AeroControls {
    float max_elevator_deg{25.f};
    float max_aileron_deg{20.f};
    float max_rudder_deg{30.f};
};

struct TvcData {
    float min_angle_deg{-20.f};
    float max_angle_deg{20.f};
    float slew_rate_deg_s{5.f};
};

struct WingSweepConfig {
    float cl_scale{1.f};
    float k_scale{1.f};
    float cd0_delta{0.f};
};

struct WingSweepData {
    float ref_sweep_deg{55.f};
    float min_deg{20.f};
    float max_deg{68.f};
    float slew_rate_deg_s{7.5f};
    Table1D schedule;       // Mach -> commanded sweep deg
    WingSweepConfig spread; // at min_deg
    WingSweepConfig swept;  // at max_deg
};

struct PropData {
    PropRotation rotation{PropRotation::CW};
    float torque_factor{0.f};
    float gyro_factor{0.f};
};

struct EngineData {
    EngineType type{EngineType::Turbofan};
    Table2D mil_thrust; // (Mach, alt_km) -> kN
    std::optional<Table2D> ab_thrust;
    float fuel_flow_idle_kg_s{0.1f};
    float fuel_flow_mil_kg_s{1.f};
    float fuel_flow_ab_kg_s{3.f};
    float spool_time_s{5.f};
};

struct CarrierData {
    float approach_m_s{69.f};
    float approach_aoa_deg{8.f};
    float cat_min_m_s{67.f};
    float hook_length_m{5.f};
};

struct RefuelingData {
    bool boom{true}; // true = boom, false = drogue
    float max_rate_kg_s{2.f};
};

struct TankerData {
    bool boom{true};
    bool drogue{false};
    int stations{1};
    float max_rate_kg_s{4.f};
    float offload_reserve{0.2f};
};

// Aggregate: everything the flight integrator needs for one aircraft type.
struct FlightModelData {
    AircraftMeta meta;
    FlightModelGeometry geometry;
    Table2D cl_table; // (alpha_deg, Mach) -> CL
    AeroDragPolar drag_polar;
    std::optional<Table1D> cd_wave; // Mach -> delta-CD
    AeroMoments moments;
    AeroLimits limits;
    AeroControls controls;
    std::optional<TvcData> tvc;
    std::optional<WingSweepData> wing_sweep;
    std::optional<PropData> prop;
    EngineData engine;
    std::optional<CarrierData> carrier;
    std::optional<RefuelingData> refueling;
    std::optional<TankerData> tanker;
};

} // namespace fl
