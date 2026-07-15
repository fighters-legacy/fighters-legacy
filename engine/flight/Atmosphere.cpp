// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/Atmosphere.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>

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
    // ── 0–20 km: the original ISA path, BIT-IDENTICAL (#354) ─────────────────
    // Every existing consumer (AeroForces, fm-trim's CI gates, the generated manual, client
    // prediction) quotes numbers from this exact sequence of operations. The high-altitude
    // extension below never touches it: same clamp, same branches, same expression order.
    if (altitude_m <= 20000.f) {
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

    // ── 20–86 km: US Standard Atmosphere 1976 (#354) ─────────────────────────
    // Gradient/isothermal layers chained from the 20 km base state computed with the SAME
    // formulas as the path above, so the boundary is continuous. Ballistic boost/coast/reentry
    // (BallisticForceModel) is the consumer; nothing that flies wings ever gets here.
    //
    //   20–32 km:  +1.0 K/km      32–47 km: +2.8 K/km     47–51 km: isothermal (270.65 K)
    //   51–71 km:  −2.8 K/km      71–86 km: −2.0 K/km     above 86 km: vacuum
    struct Layer {
        float baseAltM;
        float lapseKm; // K/m; 0 = isothermal
    };
    static constexpr Layer kLayers[] = {
        {20000.f, 0.001f}, {32000.f, 0.0028f}, {47000.f, 0.f}, {51000.f, -0.0028f}, {71000.f, -0.002f}};
    constexpr float kAtmosphereTopM = 86000.f;

    if (altitude_m >= kAtmosphereTopM) {
        // Vacuum: no pressure, no density, no aero. The speed of sound keeps the 86 km value so a
        // Mach readout stays finite (and meaningless, which is honest — there is no air).
        AtmosphereState state;
        state.pressure_pa = 0.f;
        state.density_kg_m3 = 0.f;
        state.speed_of_sound_m_s = std::sqrt(kGamma * kR * 186.87f); // T at 86 km
        return state;
    }

    // Base state at 20 km, from the same expressions the ≤20 km path uses.
    float Tb = kTtrop;
    float Pb = kP0 * std::pow(kTtrop / kT0, kExponent) * std::exp(-kG * (20000.f - kTropopause) / (kR * kTtrop));

    float T = Tb;
    float P = Pb;
    for (std::size_t i = 0; i < std::size(kLayers); ++i) {
        const float layerTop = (i + 1 < std::size(kLayers)) ? kLayers[i + 1].baseAltM : kAtmosphereTopM;
        const float h = std::min(altitude_m, layerTop);
        const float dh = h - kLayers[i].baseAltM;
        if (kLayers[i].lapseKm != 0.f) {
            T = Tb + kLayers[i].lapseKm * dh;
            P = Pb * std::pow(T / Tb, -kG / (kLayers[i].lapseKm * kR));
        } else {
            T = Tb;
            P = Pb * std::exp(-kG * dh / (kR * Tb));
        }
        if (altitude_m <= layerTop)
            break;
        Tb = T;
        Pb = P;
    }

    AtmosphereState state;
    state.pressure_pa = P;
    state.density_kg_m3 = P / (kR * T);
    state.speed_of_sound_m_s = std::sqrt(kGamma * kR * T);
    return state;
}

} // namespace fl
