// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// World Magnetic Model (WMM) declination + full geomagnetic field (#483, part of the spherical-Earth
// epic #468). Real avionics fly MAGNETIC headings, and the offset between magnetic and true north
// (declination) varies ±20°+ across the globe — the game had only true north. This wraps the standard
// degree-and-order-12 spherical-harmonic WMM synthesis (the same algorithm NOAA/NGA publish), fed by
// the bundled public-domain WMM2025 Gauss coefficients.
//
// Coordinates are geodetic (WGS84): the model does the geodetic→geocentric conversion internally, so
// callers pass the geodetic latitude/longitude the rest of the engine already uses (worldToGeodetic).

#include "flight/Geodetic.h" // LatLonAlt

#include <string>
#include <string_view>

namespace fl {

// Full geomagnetic field at a point/time. Angles in degrees; intensities in nanotesla.
struct GeoMagField {
    double declinationDeg; // D — angle of magnetic north east of true north (the compass offset)
    double inclinationDeg; // I — dip angle below horizontal
    double horizontalNt;   // H — horizontal field strength
    double totalNt;        // F — total field strength
    double northNt;        // X — geographic-north component
    double eastNt;         // Y — geographic-east component
    double downNt;         // Z — vertically-down component
};

class MagneticModel {
  public:
    // The bundled WMM2025 model (epoch 2025.0), parsed once from compiled-in coefficients. Valid for
    // dates 2025.0–2030.0; outside that range it still evaluates (with growing error) rather than fail.
    [[nodiscard]] static const MagneticModel& wmm2025();

    // Parse a NOAA/NGA WMM `.COF` coefficient file (header line + `n m g h dg dh` rows, terminated by
    // the `9999…` sentinel lines). Returns an invalid model on a malformed header.
    [[nodiscard]] static MagneticModel parseCof(std::string_view cof);

    [[nodiscard]] bool valid() const noexcept {
        return m_valid;
    }
    [[nodiscard]] double epochYear() const noexcept {
        return m_epoch;
    }

    // Full field at geodetic latitude/longitude (radians), altitude (metres above the WGS84 ellipsoid),
    // and decimal year (e.g. 2026.5). Returns a zero field on an invalid model.
    [[nodiscard]] GeoMagField field(double latRad, double lonRad, double altM, double decimalYear) const;

    // Magnetic declination only (degrees, east positive). magnetic_heading = true_heading − declination.
    [[nodiscard]] double declinationDeg(double latRad, double lonRad, double altM, double decimalYear) const;
    [[nodiscard]] double declinationDeg(LatLonAlt lla, double decimalYear) const {
        return declinationDeg(lla.lat_rad, lla.lon_rad, lla.alt_m, decimalYear);
    }

  private:
    static constexpr int kMaxOrd = 12;

    bool m_valid{false};
    double m_epoch{0.0};
    // Schmidt semi-normalised Gauss coefficients and their secular variation, folded with the
    // normalisation constants at parse time. Indexing follows the classic WMM synthesis: c[m][n]
    // carries gₙᵐ and c[n][m−1] carries hₙᵐ.
    double m_c[kMaxOrd + 1][kMaxOrd + 1]{};
    double m_cd[kMaxOrd + 1][kMaxOrd + 1]{};
    double m_k[kMaxOrd + 1][kMaxOrd + 1]{};
    double m_snorm[(kMaxOrd + 1) * (kMaxOrd + 1)]{}; // base normalisation seed (index 0 stays 1)
    double m_fn[kMaxOrd + 1]{};
    double m_fm[kMaxOrd + 1]{};
};

} // namespace fl
