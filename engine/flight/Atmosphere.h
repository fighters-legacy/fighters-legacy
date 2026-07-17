// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fl {

struct AtmosphereState {
    float density_kg_m3;
    float speed_of_sound_m_s;
    float pressure_pa;
};

// International Standard Atmosphere (ISO 2533) for 0–20 000 m — that path is BIT-IDENTICAL to the
// pre-#354 implementation, which fm-trim's CI gates and the generated manual depend on — extended
// with the US Standard Atmosphere 1976 layers to 86 000 m and vacuum above (#354, ballistic
// boost/coast/reentry). Negative altitudes clamp to sea level.
AtmosphereState computeAtmosphere(float altitude_m);

// ── Airspeed distinctions (#480) ─────────────────────────────────────────────
// One home for the pitot-static conversions the HUD, flight model and manual all need. Every
// aircraft carries several distinct "speeds" that diverge with altitude, and conflating them is a
// classic sim bug (the HUD used to label groundspeed "IAS"): true airspeed (TAS, motion relative to
// the air mass), groundspeed (motion over the ground = TAS ± wind), Mach (TAS ÷ local speed of
// sound), and indicated/calibrated airspeed (what a pitot-static gauge reads — TAS collapsed toward
// sea-level dynamic pressure, so a fixed IAS means a fixed g-margin regardless of altitude).

// Sea-level ISA reference density (kg/m³) and speed of sound (m/s) — the datum EAS/CAS collapse to.
constexpr float kSeaLevelDensity = 1.2250f;
constexpr float kSeaLevelSpeedOfSound = 340.294f;
constexpr float kSeaLevelPressurePa = 101325.f;

// Mach number: TAS ÷ local speed of sound. 0 when the speed of sound is non-positive (vacuum
// ceiling) — matches the guarded `spd/a` idiom the aero path already uses, so it is a drop-in.
[[nodiscard]] float machNumber(float tas_m_s, float speedOfSound_m_s) noexcept;

// Incompressible dynamic pressure q = ½·ρ·V² (Pa).
[[nodiscard]] float dynamicPressurePa(float density_kg_m3, float tas_m_s) noexcept;

// Equivalent airspeed: TAS scaled by √(ρ/ρ₀). The incompressible density collapse — same dynamic
// pressure, hence the same aerodynamic loading, as EAS at sea level.
[[nodiscard]] float equivalentAirspeed(float tas_m_s, float density_kg_m3) noexcept;

// Calibrated airspeed (m/s) — what a perfect pitot-static airspeed indicator reads. Forms the
// compressible impact pressure qc at the local static condition (subsonic vs. supersonic/Rayleigh
// branch on local Mach), then inverts the sea-level relation to a speed. Equals TAS at sea level in
// still air; below it aloft. Indicated airspeed equals this minus instrument/position error (none
// modelled), so the HUD treats CAS as IAS.
[[nodiscard]] float calibratedAirspeed(float tas_m_s, const AtmosphereState& local) noexcept;

// Pressure altitude (m): the ISA altitude at which static pressure equals `pressure_pa` — the
// altimeter reading at the 29.92 inHg / 1013.25 hPa standard datum. Inverts the ISA P(h) profile
// (troposphere + isothermal lower stratosphere; saturates at the 20 km model ceiling).
[[nodiscard]] float pressureAltitudeM(float pressure_pa) noexcept;

} // namespace fl
