// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/AeroForces.h"
#include "flight/FlightModelData.h"

#include <memory>

namespace fl {

// Aircraft state vector — flat/POD for network serialisation.
struct FlightState {
    float pos_world[3]{};          // world-frame position (m)
    float vel_body[3]{};           // body-frame velocity (m/s)
    float euler[3]{};              // roll, pitch, yaw (rad) — derived from quaternion
    float omega[3]{};              // body-frame angular rates: p, q, r (rad/s)
    float quat[4]{0, 0, 0, 1};     // body↔world quaternion [x,y,z,w]
    float mass_kg{10000.f};        // current total mass (decreases as fuel burns)
    float fuel_kg{4000.f};         // remaining fuel
    float throttle_actual{0.f};    // actual throttle after spool lag [0,1]
    float current_sweep_deg{55.f}; // current wing sweep angle (fixed-geometry: equals ref_sweep_deg)
    bool ab_engaged{false};
    float tvc_angle_deg{0.f}; // current TVC nozzle angle
};

class FlightIntegrator {
  public:
    explicit FlightIntegrator(std::shared_ptr<const FlightModelData> data);

    // Reset to a new initial state (e.g. spawn or respawn).
    void reset(const FlightState& state);

    // Advance the simulation by dt seconds.
    // ctrl: pilot or AI inputs for this tick.
    // payload: weapon mass and drag contribution for this tick.
    void step(float dt, const ControlInput& ctrl, const PayloadEffect& payload);

    [[nodiscard]] const FlightState& state() const {
        return m_state;
    }

  private:
    std::shared_ptr<const FlightModelData> m_data;
    FlightState m_state;

    void advanceSpool(float dt, float commanded_throttle);
    void advanceSweep(float dt, float commanded_sweep_deg);
    void advanceTvc(float dt, float commanded_tvc_deg);
    void integrateRotation(float dt);
};

} // namespace fl
