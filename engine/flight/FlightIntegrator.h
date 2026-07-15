// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/AeroForces.h"
#include "flight/CentralGravityField.h"
#include "flight/FixedWingForceModel.h"
#include "flight/FlightModelData.h"
#include "flight/IGravityField.h"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace fl {

// Aircraft state vector — flat/POD for network serialisation.
struct FlightState {
    double pos_world[3]{};         // world-frame position (m) — double for planet-scale precision
    double vel_body[3]{};          // body-frame velocity (m/s) — double for ICBM-range precision
    float euler[3]{};              // roll, pitch, yaw (rad) — derived from quaternion
    float omega[3]{};              // body-frame angular rates: p, q, r (rad/s)
    float quat[4]{0, 0, 0, 1};     // body↔world quaternion [x,y,z,w]
    float mass_kg{10000.f};        // current total mass (decreases as fuel burns)
    float fuel_kg{4000.f};         // remaining fuel
    float throttle_actual{0.f};    // actual throttle after spool lag [0,1]
    float current_sweep_deg{55.f}; // current wing sweep angle (fixed-geometry: equals ref_sweep_deg)
    bool ab_engaged{false};
    uint8_t engineFailFlags{0}; // fl::kEngineFail* bitmask; drives asymmetric thrust (#675)
    float tvc_angle_deg{0.f};   // current TVC nozzle angle

    // ── [aero.limits] enforcement outputs (#816) ─────────────────────────────
    // These are OUTPUTS of step(), not inputs. Until now alpha_stall_deg, max_g_structural and
    // min_g_structural were parsed, required, and read by absolutely nothing.

    // True when alpha exceeds limits.alpha_stall_deg. The CL collapse itself is NOT applied here —
    // the cl_table already carries it if the author wrote an honest one, and clamping CL on top would
    // double-count the stall (and quietly reward an author who did not). This flag drives buffet, the
    // HUD cue, and audio; it does not change the aerodynamics.
    bool stalled{false};

    // Normal (body-y) load factor in g. n = aero_force_y / (eff_mass * g0), so it excludes gravity —
    // this is what an accelerometer in the cockpit reads, and what a G-meter shows.
    float load_factor{1.f};

    // Cumulative seconds spent beyond kOverGMargin past the structural limit. Reset when back inside.
    float overg_seconds{0.f};

    // ONE-SHOT: set true by step() on the tick an over-G damage event fires, and cleared on the next
    // step(). The integrator does NOT apply the damage itself: EntityManager::applyDamage fires event
    // handlers, and the integrate pass is data-parallel, so calling it from a worker would be a data
    // race. WorldBroadcaster latches this flag and applies the damage serially after the pass.
    bool overg_damage{false};

    // ONE-SHOT (#626): the radial impact speed (m/s) of a hard ground contact this tick, 0 when
    // none. Same discipline as overg_damage — the integrator only reports; WorldBroadcaster applies
    // crash damage serially after the parallel pass, gated by the crashDamage difficulty toggle.
    // Ordinary landings stay below the reporting threshold and never raise it.
    float ground_impact_speed{0.f};
};

// Wind and turbulence injected each tick by WorldBroadcaster from WeatherController state.
struct WindInfluence {
    float wind_world[3]{};      // steady wind + gust, world frame (m/s); Y component is 0
    float turbulence_body[3]{}; // per-tick stochastic buffeting, body frame (m/s)
};

class FlightIntegrator {
  public:
    explicit FlightIntegrator(std::shared_ptr<const FlightModelData> data);

    // Reset to a new initial state (e.g. spawn or respawn).
    void reset(const FlightState& state);

    // Advance the simulation by dt seconds.
    // ctrl:       pilot or AI inputs for this tick.
    // payload:    weapon mass and drag contribution for this tick.
    // wind:       optional weather perturbation; zero-initialised default = no wind effect.
    // groundElev: terrain elevation (m) ABOVE THE DATUM along the radial through the aircraft —
    //             i.e. TerrainStreamer::heightAt(dvec3). Ground contact compares it against the
    //             geodetic (MSL) altitude and snaps along the local radial up, so collision is
    //             correct anywhere on the planet (#477); 0 = sea-level (datum) floor.
    void step(float dt, const ControlInput& ctrl, const PayloadEffect& payload, const WindInfluence& wind = {},
              float groundElev = 0.f);

    [[nodiscard]] const FlightState& state() const {
        return m_state;
    }

    // The model this integrator is flying. Callers reacting to an over-G event (#816) need its
    // structural limit to scale the damage; nobody should be re-deriving it from the entity def.
    [[nodiscard]] const FlightModelData& flightModel() const {
        return *m_data;
    }

    // Inject an alternative gravity field (default: CentralGravityField::earthInstance()).
    // A custom field plugs in here for exotic planets without touching step().
    void setGravityField(const IGravityField& field) {
        m_gravity = &field;
    }

    // Inject an alternative force model (default: FixedWingForceModel). A multirotor/rotor-disc or
    // ballistic point-mass model plugs in here without touching the integrator's F=ma core.
    void setForceModel(const IForceModel& model) {
        m_forceModel = &model;
    }

    // The NaN/overflow backstop on body velocity (#354) — NOT a top-speed limiter (max_mach is
    // enforced by fm-trim in CI). Default 2000 m/s (≈ Mach 6 at sea level) suits every winged
    // vehicle; ballistic entities set ~8000 because an MRBM legitimately flies faster than the
    // guard built for aircraft.
    void setSpeedGuard(double mps) noexcept {
        if (mps > 0.0)
            m_speedGuardMps = mps;
    }

    // Progressive damage penalties (#626) — this is where DamageDef's thrustFactor/controlFactor
    // finally act on the physics. Applied to the COMMANDED inputs at the top of step(): thrust
    // scales the throttle command, control scales surface deflection commands. Both clamped to
    // [0, 1]; (1, 1) = undamaged. WorldBroadcaster sets them on DamageLevelChanged.
    void setDamagePenalty(float thrustFactor, float controlFactor) noexcept {
        m_damageThrust = std::clamp(thrustFactor, 0.f, 1.f);
        m_damageControl = std::clamp(controlFactor, 0.f, 1.f);
    }
    [[nodiscard]] float damageThrustFactor() const noexcept {
        return m_damageThrust;
    }
    [[nodiscard]] float damageControlFactor() const noexcept {
        return m_damageControl;
    }

    // Per-subsystem damage effects (#675), independent of the tier penalties above so the two layer
    // rather than overwrite. Engine-out flags drive the force model's asymmetric thrust; the
    // subsystem control factor (controls + hydraulics losses) multiplies the tier control factor; a
    // fuel leak drains on top of the burn.
    void setEngineFailFlags(uint8_t flags) noexcept {
        m_state.engineFailFlags = flags;
    }
    [[nodiscard]] uint8_t engineFailFlags() const noexcept {
        return m_state.engineFailFlags;
    }
    void setSubsystemControlFactor(float factor) noexcept {
        m_subsystemControl = std::clamp(factor, 0.f, 1.f);
    }
    void setFuelLeakRate(float kgPerS) noexcept {
        m_fuelLeakKgS = std::max(0.f, kgPerS);
    }

  private:
    std::shared_ptr<const FlightModelData> m_data;
    FlightState m_state;
    double m_speedGuardMps{2000.0}; // see setSpeedGuard (#354)
    const IGravityField* m_gravity{&CentralGravityField::earthInstance()};
    const IForceModel* m_forceModel{&FixedWingForceModel::instance()};
    float m_damageThrust{1.f};
    float m_damageControl{1.f};
    float m_subsystemControl{1.f}; // #675: controls/hydraulics loss, multiplies m_damageControl
    float m_fuelLeakKgS{0.f};      // #675: ruptured-tank drain on top of the burn

    void advanceSpool(float dt, float commanded_throttle);
    void advanceSweep(float dt, float commanded_sweep_deg);
    void advanceTvc(float dt, float commanded_tvc_deg);
    void integrateRotation(float dt);
};

} // namespace fl
