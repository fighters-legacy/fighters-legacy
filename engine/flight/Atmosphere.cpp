// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/Atmosphere.h"

#include <algorithm>
#include <cmath>

namespace fl {

namespace {

constexpr float kT0 = 288.15f;                           // sea-level temperature (K)
constexpr float kP0 = 101325.f;                          // sea-level pressure (Pa)
constexpr float kLapseRate = 0.0065f;                    // temperature lapse rate, troposphere (K/m)
constexpr float kTropopause = 11000.f;                   // tropopause altitude (m)
constexpr float kTtrop = kT0 - kLapseRate * kTropopause; // temp at tropopause (K)
constexpr float kGamma = 1.4f;                           // ratio of specific heats
constexpr float kR = 287.05f;                            // specific gas constant for dry air (J/(kg·K))
constexpr float kG = 9.80665f;                           // standard gravity (m/s²)

constexpr float kExponent = kG / (kLapseRate * kR); // ISA troposphere pressure exponent

} // namespace

AtmosphereState computeAtmosphere(float altitude_m) {
    altitude_m = std::clamp(altitude_m, 0.f, 20000.f);

    float T, P;

    if (altitude_m <= kTropopause) {
        // Troposphere: linear temperature decrease
        T = kT0 - kLapseRate * altitude_m;
        P = kP0 * std::pow(T / kT0, kExponent);
    } else {
        // Lower stratosphere: isothermal (constant temperature)
        float Ptrop = kP0 * std::pow(kTtrop / kT0, kExponent);
        T = kTtrop;
        P = Ptrop * std::exp(-kG * (altitude_m - kTropopause) / (kR * kTtrop));
    }

    AtmosphereState state;
    state.pressure_pa = P;
    state.density_kg_m3 = P / (kR * T);
    state.speed_of_sound_m_s = std::sqrt(kGamma * kR * T);
    return state;
}

} // namespace fl
