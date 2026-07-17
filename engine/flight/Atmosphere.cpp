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

// ── Airspeed distinctions (#480) ─────────────────────────────────────────────

float machNumber(float tas_m_s, float speedOfSound_m_s) noexcept {
    return speedOfSound_m_s > 0.f ? tas_m_s / speedOfSound_m_s : 0.f;
}

float dynamicPressurePa(float density_kg_m3, float tas_m_s) noexcept {
    return 0.5f * density_kg_m3 * tas_m_s * tas_m_s;
}

float equivalentAirspeed(float tas_m_s, float density_kg_m3) noexcept {
    if (density_kg_m3 <= 0.f)
        return 0.f;
    return tas_m_s * std::sqrt(density_kg_m3 / kSeaLevelDensity);
}

float calibratedAirspeed(float tas_m_s, const AtmosphereState& local) noexcept {
    // Above the atmosphere (vacuum) or a degenerate state: no impact pressure, no reading.
    if (local.pressure_pa <= 0.f || local.speed_of_sound_m_s <= 0.f || tas_m_s <= 0.f)
        return 0.f;

    // Local Mach selects the compressible impact-pressure (qc) relation. γ = 1.4:
    //   subsonic:   qc = P·[(1 + 0.2·M²)^3.5 − 1]
    //   supersonic: qc = P·[166.9215801·M⁷ / (7·M² − 1)^2.5 − 1]   (Rayleigh pitot)
    const float M = tas_m_s / local.speed_of_sound_m_s;
    float qc;
    if (M <= 1.f) {
        qc = local.pressure_pa * (std::pow(1.f + 0.2f * M * M, 3.5f) - 1.f);
    } else {
        const float m2 = M * M;
        qc = local.pressure_pa * (166.9215801f * std::pow(M, 7.f) / std::pow(7.f * m2 - 1.f, 2.5f) - 1.f);
    }

    // Invert qc to a speed at the sea-level datum (P0, a0). The subsonic inversion holds while the
    // result is itself subsonic (CAS ≤ a0); beyond that the datum is also supersonic, so Newton-solve
    // the Rayleigh relation for the calibrated Mach mc.
    const float subsonicCas =
        kSeaLevelSpeedOfSound * std::sqrt(5.f * (std::pow(qc / kSeaLevelPressurePa + 1.f, 2.f / 7.f) - 1.f));
    if (subsonicCas <= kSeaLevelSpeedOfSound)
        return subsonicCas;

    // Supersonic datum: solve qc/P0 + 1 = 166.9215801·mc⁷ / (7·mc² − 1)^2.5 for mc ≥ 1.
    const float rhs = qc / kSeaLevelPressurePa + 1.f;
    float mc = M; // local Mach is a good seed
    for (int i = 0; i < 12; ++i) {
        const float mc2 = mc * mc;
        const float denom = std::pow(7.f * mc2 - 1.f, 2.5f);
        const float f = 166.9215801f * std::pow(mc, 7.f) / denom - rhs;
        // f'(mc) = 166.9215801·mc⁶·(7·mc² − 7) / (7·mc² − 1)^3.5  (quotient rule, simplified)
        const float fp = 166.9215801f * std::pow(mc, 6.f) * (7.f * mc2 - 7.f) / std::pow(7.f * mc2 - 1.f, 3.5f);
        if (fp == 0.f)
            break;
        const float step = f / fp;
        mc -= step;
        if (mc < 1.f)
            mc = 1.f;
        if (std::fabs(step) < 1e-6f)
            break;
    }
    return mc * kSeaLevelSpeedOfSound;
}

float pressureAltitudeM(float pressure_pa) noexcept {
    if (pressure_pa >= kP0)
        return 0.f;
    // Pressure at the tropopause on the ISA profile.
    const float Ptrop = kP0 * std::pow(kTtrop / kT0, kExponent);
    if (pressure_pa >= Ptrop) {
        // Troposphere: P = P0·(1 − L·h/T0)^(g/(L·R))  ⇒  h = (T0/L)·(1 − (P/P0)^(L·R/g)).
        return (kT0 / kLapseRate) * (1.f - std::pow(pressure_pa / kP0, (kLapseRate * kR) / kG));
    }
    // Isothermal lower stratosphere (11–20 km): P = Ptrop·exp(−g·(h−11000)/(R·Ttrop)).
    const float h = kTropopause + (kR * kTtrop / kG) * std::log(Ptrop / pressure_pa);
    return std::min(h, 20000.f); // saturate at the modelled ceiling
}

} // namespace fl
