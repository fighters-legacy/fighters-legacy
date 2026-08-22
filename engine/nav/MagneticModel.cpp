// SPDX-License-Identifier: GPL-3.0-or-later
#include "nav/MagneticModel.h"

#include "math/Angles.h"

#include <cmath>
#include <sstream>

namespace fl {

namespace {

// WGS84 ellipsoid squared axes (km²) and geomagnetic reference radius (km), per the WMM report.
constexpr double kA2 = 40680631.59;
constexpr double kB2 = 40408299.98;
constexpr double kC2 = kA2 - kB2;
constexpr double kA4 = kA2 * kA2;
constexpr double kC4 = kA4 - kB2 * kB2;
constexpr double kReKm = 6371.2;

// The bundled public-domain WMM2025 Gauss coefficients (NOAA/NGA, epoch 2025.0). Kept as a compiled-in
// string so the model is self-contained (no runtime file dependency); the identical human-readable copy
// ships as data/WMM.COF. parseCof accepts either.
constexpr const char* kWmm2025Cof = R"COF(    2025.0            WMM-2025        11/13/2024
  1  0  -29351.8       0.0       12.0        0.0
  1  1   -1410.8    4545.4        9.7      -21.5
  2  0   -2556.6       0.0      -11.6        0.0
  2  1    2951.1   -3133.6       -5.2      -27.7
  2  2    1649.3    -815.1       -8.0      -12.1
  3  0    1361.0       0.0       -1.3        0.0
  3  1   -2404.1     -56.6       -4.2        4.0
  3  2    1243.8     237.5        0.4       -0.3
  3  3     453.6    -549.5      -15.6       -4.1
  4  0     895.0       0.0       -1.6        0.0
  4  1     799.5     278.6       -2.4       -1.1
  4  2      55.7    -133.9       -6.0        4.1
  4  3    -281.1     212.0        5.6        1.6
  4  4      12.1    -375.6       -7.0       -4.4
  5  0    -233.2       0.0        0.6        0.0
  5  1     368.9      45.4        1.4       -0.5
  5  2     187.2     220.2        0.0        2.2
  5  3    -138.7    -122.9        0.6        0.4
  5  4    -142.0      43.0        2.2        1.7
  5  5      20.9     106.1        0.9        1.9
  6  0      64.4       0.0       -0.2        0.0
  6  1      63.8     -18.4       -0.4        0.3
  6  2      76.9      16.8        0.9       -1.6
  6  3    -115.7      48.8        1.2       -0.4
  6  4     -40.9     -59.8       -0.9        0.9
  6  5      14.9      10.9        0.3        0.7
  6  6     -60.7      72.7        0.9        0.9
  7  0      79.5       0.0       -0.0        0.0
  7  1     -77.0     -48.9       -0.1        0.6
  7  2      -8.8     -14.4       -0.1        0.5
  7  3      59.3      -1.0        0.5       -0.8
  7  4      15.8      23.4       -0.1        0.0
  7  5       2.5      -7.4       -0.8       -1.0
  7  6     -11.1     -25.1       -0.8        0.6
  7  7      14.2      -2.3        0.8       -0.2
  8  0      23.2       0.0       -0.1        0.0
  8  1      10.8       7.1        0.2       -0.2
  8  2     -17.5     -12.6        0.0        0.5
  8  3       2.0      11.4        0.5       -0.4
  8  4     -21.7      -9.7       -0.1        0.4
  8  5      16.9      12.7        0.3       -0.5
  8  6      15.0       0.7        0.2       -0.6
  8  7     -16.8      -5.2       -0.0        0.3
  8  8       0.9       3.9        0.2        0.2
  9  0       4.6       0.0       -0.0        0.0
  9  1       7.8     -24.8       -0.1       -0.3
  9  2       3.0      12.2        0.1        0.3
  9  3      -0.2       8.3        0.3       -0.3
  9  4      -2.5      -3.3       -0.3        0.3
  9  5     -13.1      -5.2        0.0        0.2
  9  6       2.4       7.2        0.3       -0.1
  9  7       8.6      -0.6       -0.1       -0.2
  9  8      -8.7       0.8        0.1        0.4
  9  9     -12.9      10.0       -0.1        0.1
 10  0      -1.3       0.0        0.1        0.0
 10  1      -6.4       3.3        0.0        0.0
 10  2       0.2       0.0        0.1       -0.0
 10  3       2.0       2.4        0.1       -0.2
 10  4      -1.0       5.3       -0.0        0.1
 10  5      -0.6      -9.1       -0.3       -0.1
 10  6      -0.9       0.4        0.0        0.1
 10  7       1.5      -4.2       -0.1        0.0
 10  8       0.9      -3.8       -0.1       -0.1
 10  9      -2.7       0.9       -0.0        0.2
 10 10      -3.9      -9.1       -0.0       -0.0
 11  0       2.9       0.0        0.0        0.0
 11  1      -1.5       0.0       -0.0       -0.0
 11  2      -2.5       2.9        0.0        0.1
 11  3       2.4      -0.6        0.0       -0.0
 11  4      -0.6       0.2        0.0        0.1
 11  5      -0.1       0.5       -0.1       -0.0
 11  6      -0.6      -0.3        0.0       -0.0
 11  7      -0.1      -1.2       -0.0        0.1
 11  8       1.1      -1.7       -0.1       -0.0
 11  9      -1.0      -2.9       -0.1        0.0
 11 10      -0.2      -1.8       -0.1        0.0
 11 11       2.6      -2.3       -0.1        0.0
 12  0      -2.0       0.0        0.0        0.0
 12  1      -0.2      -1.3        0.0       -0.0
 12  2       0.3       0.7       -0.0        0.0
 12  3       1.2       1.0       -0.0       -0.1
 12  4      -1.3      -1.4       -0.0        0.1
 12  5       0.6      -0.0       -0.0       -0.0
 12  6       0.6       0.6        0.1       -0.0
 12  7       0.5      -0.1       -0.0       -0.0
 12  8      -0.1       0.8        0.0        0.0
 12  9      -0.4       0.1        0.0       -0.0
 12 10      -0.2      -1.0       -0.1       -0.0
 12 11      -1.3       0.1       -0.0        0.0
 12 12      -0.7       0.2       -0.1       -0.1
)COF";

} // namespace

MagneticModel MagneticModel::parseCof(std::string_view cof) {
    MagneticModel m;
    constexpr int N = kMaxOrd;

    std::istringstream in{std::string(cof)};
    std::string line;
    if (!std::getline(in, line))
        return m;
    { // header: the first token is the epoch (decimal year)
        std::istringstream hs{line};
        if (!(hs >> m.m_epoch) || m.m_epoch < 1900.0 || m.m_epoch > 2100.0)
            return m;
    }

    // Read raw Gauss coefficients into c[m][n] (g) and c[n][m-1] (h) — the classic WMM storage.
    while (std::getline(in, line)) {
        std::istringstream ls{line};
        int n, mm;
        double gnm, hnm, dgnm, dhnm;
        if (!(ls >> n >> mm >> gnm >> hnm >> dgnm >> dhnm))
            break; // the 9999… sentinel / blank tail stops us
        if (n < 1 || n > N || mm < 0 || mm > n)
            continue;
        m.m_c[mm][n] = gnm;
        m.m_cd[mm][n] = dgnm;
        if (mm != 0) {
            m.m_c[n][mm - 1] = hnm;
            m.m_cd[n][mm - 1] = dhnm;
        }
    }

    // Fold the Schmidt semi-normalisation into the coefficients (done once).
    m.m_snorm[0] = 1.0;
    m.m_fm[0] = 0.0;
    for (int n = 1; n <= N; ++n) {
        m.m_snorm[n] = m.m_snorm[n - 1] * static_cast<double>(2 * n - 1) / static_cast<double>(n);
        int j = 2;
        for (int mm = 0; mm <= n; ++mm) {
            m.m_k[mm][n] =
                static_cast<double>((n - 1) * (n - 1) - mm * mm) / static_cast<double>((2 * n - 1) * (2 * n - 3));
            if (mm > 0) {
                const double flnmj = static_cast<double>((n - mm + 1) * j) / static_cast<double>(n + mm);
                m.m_snorm[n + mm * 13] = m.m_snorm[n + (mm - 1) * 13] * std::sqrt(flnmj);
                j = 1;
                m.m_c[n][mm - 1] *= m.m_snorm[n + mm * 13];
                m.m_cd[n][mm - 1] *= m.m_snorm[n + mm * 13];
            }
            m.m_c[mm][n] *= m.m_snorm[n + mm * 13];
            m.m_cd[mm][n] *= m.m_snorm[n + mm * 13];
        }
        m.m_fn[n] = static_cast<double>(n + 1);
        m.m_fm[n] = static_cast<double>(n);
    }
    m.m_k[1][1] = 0.0;

    m.m_valid = true;
    return m;
}

const MagneticModel& MagneticModel::wmm2025() {
    static const MagneticModel model = parseCof(kWmm2025Cof);
    return model;
}

GeoMagField MagneticModel::field(double latRad, double lonRad, double altM, double decimalYear) const {
    GeoMagField out{};
    if (!m_valid)
        return out;

    constexpr int N = kMaxOrd;
    const double dt = decimalYear - m_epoch;
    const double glat = latRad / kDegToRad<double>; // the algorithm below works in degrees/km
    const double alt = altM / 1000.0;

    const double srlon = std::sin(lonRad), crlon = std::cos(lonRad);
    const double srlat = std::sin(latRad), crlat = std::cos(latRad);
    const double srlat2 = srlat * srlat, crlat2 = crlat * crlat;
    (void)glat;

    // Per-evaluation scratch (keeps field() const + thread-safe — the coefficients are the only state).
    double p[(N + 1) * (N + 1)]{};
    double dp[N + 1][N + 1]{};
    double pp[N + 1]{};
    double sp[N + 1]{}, cp[N + 1]{};
    double tc[N + 1][N + 1]{};
    p[0] = 1.0;
    pp[0] = 1.0;
    sp[0] = 0.0;
    cp[0] = 1.0;
    sp[1] = srlon;
    cp[1] = crlon;

    // Geodetic → geocentric spherical.
    const double q = std::sqrt(kA2 - kC2 * srlat2);
    const double q1 = alt * q;
    const double q2 = ((q1 + kA2) / (q1 + kB2)) * ((q1 + kA2) / (q1 + kB2));
    const double ct = srlat / std::sqrt(q2 * crlat2 + srlat2);
    const double st = std::sqrt(1.0 - ct * ct);
    const double r2 = alt * alt + 2.0 * q1 + (kA4 - kC4 * srlat2) / (q * q);
    const double r = std::sqrt(r2);
    const double d = std::sqrt(kA2 * crlat2 + kB2 * srlat2);
    const double ca = (alt + d) / r;
    const double sa = kC2 * crlat * srlat / (r * d);

    for (int mm = 2; mm <= N; ++mm) {
        sp[mm] = sp[1] * cp[mm - 1] + cp[1] * sp[mm - 1];
        cp[mm] = cp[1] * cp[mm - 1] - sp[1] * sp[mm - 1];
    }

    const double aor = kReKm / r;
    double ar = aor * aor;
    double br = 0.0, bt = 0.0, bp = 0.0, bpp = 0.0;

    for (int n = 1; n <= N; ++n) {
        ar *= aor;
        for (int mm = 0; mm <= n; ++mm) {
            // Schmidt semi-normalised associated Legendre P and its θ-derivative, by recursion.
            if (n == mm) {
                p[n + mm * 13] = st * p[n - 1 + (mm - 1) * 13];
                dp[mm][n] = st * dp[mm - 1][n - 1] + ct * p[n - 1 + (mm - 1) * 13];
            } else if (n == 1 && mm == 0) {
                p[n + mm * 13] = ct * p[n - 1 + mm * 13];
                dp[mm][n] = ct * dp[mm][n - 1] - st * p[n - 1 + mm * 13];
            } else if (n > 1 && n != mm) {
                if (mm > n - 2) {
                    p[n - 2 + mm * 13] = 0.0;
                    dp[mm][n - 2] = 0.0;
                }
                p[n + mm * 13] = ct * p[n - 1 + mm * 13] - m_k[mm][n] * p[n - 2 + mm * 13];
                dp[mm][n] = ct * dp[mm][n - 1] - st * p[n - 1 + mm * 13] - m_k[mm][n] * dp[mm][n - 2];
            }

            // Time-adjust the coefficients to the requested year.
            tc[mm][n] = m_c[mm][n] + dt * m_cd[mm][n];
            if (mm != 0)
                tc[n][mm - 1] = m_c[n][mm - 1] + dt * m_cd[n][mm - 1];

            const double par = ar * p[n + mm * 13];
            double temp1, temp2;
            if (mm == 0) {
                temp1 = tc[mm][n] * cp[mm];
                temp2 = tc[mm][n] * sp[mm];
            } else {
                temp1 = tc[mm][n] * cp[mm] + tc[n][mm - 1] * sp[mm];
                temp2 = tc[mm][n] * sp[mm] - tc[n][mm - 1] * cp[mm];
            }
            bt -= ar * temp1 * dp[mm][n];
            bp += m_fm[mm] * temp2 * par;
            br += m_fn[n] * temp1 * par;

            // Geographic-pole special case (st == 0): accumulate the east component separately.
            if (st == 0.0 && mm == 1) {
                pp[n] = (n == 1) ? pp[n - 1] : pp[n - 1] * ct - m_k[mm][n] * pp[n - 2];
                const double parp = ar * pp[n];
                bpp += m_fm[mm] * temp2 * parp;
            }
        }
    }

    if (st == 0.0)
        bp = bpp;
    else
        bp /= st;

    // Rotate spherical → geodetic field components.
    const double bx = -bt * ca - br * sa; // north (X)
    const double by = bp;                 // east  (Y)
    const double bz = bt * sa - br * ca;  // down  (Z)

    out.northNt = bx;
    out.eastNt = by;
    out.downNt = bz;
    out.horizontalNt = std::sqrt(bx * bx + by * by);
    out.totalNt = std::sqrt(out.horizontalNt * out.horizontalNt + bz * bz);
    out.declinationDeg = std::atan2(by, bx) / kDegToRad<double>;
    out.inclinationDeg = std::atan2(bz, out.horizontalNt) / kDegToRad<double>;
    return out;
}

double MagneticModel::declinationDeg(double latRad, double lonRad, double altM, double decimalYear) const {
    return field(latRad, lonRad, altM, decimalYear).declinationDeg;
}

} // namespace fl
