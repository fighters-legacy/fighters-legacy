// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The two smallest entity builders, shared by suites that have nothing else in common (#1276).
//
// Deliberately NOT in ai_flight_harness.h, which is where an EntityState builder would naturally
// go: that header pulls FlightIntegrator and BuiltinFlightModel, and the landing/takeoff suites
// link engine-ai/entity/spatial and no engine-flight. A shared home is only shared if the NARROWEST
// consumer can reach it, so this one is over entity/ alone.

#include "entity/EntityDef.h"
#include "entity/EntityState.h"

#include <string>
#include <utility>
#include <vector>

namespace fl {

// State at (px, alt, pz) with world velocity (vx, vy, vz) and identity attitude (nose = +X world).
// Near the world origin that is the north pole, where local up = +Y and localAltitude == alt, and
// East = +X — so a runway heading of 90 degrees points along +X, down the nose.
[[nodiscard]] inline EntityState mkState(double px, double alt, double pz, float vx, float vy, float vz) {
    EntityState s{};
    s.id = {1, 1};
    s.transform.pos[0] = px;
    s.transform.pos[1] = alt;
    s.transform.pos[2] = pz;
    s.transform.vel[0] = vx;
    s.transform.vel[1] = vy;
    s.transform.vel[2] = vz;
    s.transform.quat[3] = 1.f; // identity
    return s;
}

// The minimal air-vehicle def the sensor suites register: a name, a category, some hp, and whatever
// sensor ids the case is about.
[[nodiscard]] inline EntityDef makeSensorCarrierDef(std::string id, std::vector<std::string> sensors = {}) {
    EntityDef d;
    d.id = std::move(id);
    d.name = "T";
    d.category = ObjectCategory::AirVehicle;
    d.maxHp = 100.f;
    d.sensorIds = std::move(sensors);
    return d;
}

} // namespace fl
