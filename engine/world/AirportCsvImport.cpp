// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/AirportCsvImport.h"
#include "util/Parse.h"
#include "util/Str.h"

#include "math/Angles.h"
#include "math/Units.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace fl {

namespace {

// ── RFC-4180 CSV ──────────────────────────────────────────────────────────────
// A minimal, allocation-light parser: yields one row (vector<string>) at a time. Handles quoted
// fields with embedded commas, escaped quotes (""), and CRLF/LF line endings; a quoted field may
// contain newlines. Sufficient for the OurAirports files.
class CsvReader {
  public:
    explicit CsvReader(std::string_view text) : m_text(text) {}

    // Reads the next row into `out`; returns false at end of input.
    bool next(std::vector<std::string>& out) {
        out.clear();
        if (m_pos >= m_text.size())
            return false;
        std::string field;
        bool inQuotes = false;
        bool any = false;
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            any = true;
            if (inQuotes) {
                if (c == '"') {
                    if (m_pos < m_text.size() && m_text[m_pos] == '"') {
                        field.push_back('"');
                        ++m_pos;
                    } else {
                        inQuotes = false;
                    }
                } else {
                    field.push_back(c);
                }
            } else if (c == '"') {
                inQuotes = true;
            } else if (c == ',') {
                out.push_back(std::move(field));
                field.clear();
            } else if (c == '\n') {
                break;
            } else if (c == '\r') {
                // swallow; the \n (if any) ends the row
            } else {
                field.push_back(c);
            }
        }
        out.push_back(std::move(field));
        return any;
    }

  private:
    std::string_view m_text;
    std::size_t m_pos{0};
};

// Column-name -> index map from a header row.
using ColumnMap = std::unordered_map<std::string, std::size_t>;
[[nodiscard]] ColumnMap headerMap(const std::vector<std::string>& header) {
    ColumnMap m;
    for (std::size_t i = 0; i < header.size(); ++i)
        m[header[i]] = i;
    return m;
}
[[nodiscard]] std::string_view cell(const std::vector<std::string>& row, const ColumnMap& cols, const char* name) {
    const auto it = cols.find(name);
    if (it == cols.end() || it->second >= row.size())
        return {};
    return row[it->second];
}

// Deliberately the TOLERANT parser (#1244): OurAirports fields carry unit suffixes and stray
// characters, and a row that reads "1500 ft" should import as 1500, not be dropped.
[[nodiscard]] bool parseDouble(std::string_view s, double& out) {
    return readInto(parseLeadingDouble(trim(s)), out);
}

[[nodiscard]] std::string upper(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return r;
}
[[nodiscard]] bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

// Bearing (deg, 0=N, 90=E) from (lat0,lon0) to (lat1,lon1), all radians.
[[nodiscard]] double initialBearingDeg(double lat0, double lon0, double lat1, double lon1) {
    const double dLon = lon1 - lon0;
    const double y = std::sin(dLon) * std::cos(lat1);
    const double x = std::cos(lat0) * std::sin(lat1) - std::sin(lat0) * std::cos(lat1) * std::cos(dLon);
    double b = std::atan2(y, x) / kDegToRad<double>;
    b = std::fmod(b + 360.0, 360.0);
    return b;
}

// Heading from a runway ident like "09", "27L", "9" -> degrees (ident number x 10).
[[nodiscard]] bool headingFromIdent(std::string_view ident, double& out) {
    int num = 0;
    bool any = false;
    for (char c : ident) {
        if (c >= '0' && c <= '9') {
            num = num * 10 + (c - '0');
            any = true;
        } else {
            break;
        }
    }
    if (!any || num <= 0 || num > 36)
        return false;
    out = static_cast<double>(num) * 10.0;
    return true;
}

} // namespace

RunwaySurface runwaySurfaceFromOurAirports(std::string_view surface) noexcept {
    const std::string s = upper(surface);
    if (startsWith(s, "CON") || startsWith(s, "CEM") || startsWith(s, "PEM"))
        return RunwaySurface::Concrete;
    if (startsWith(s, "ASP") || startsWith(s, "BIT") || startsWith(s, "PAV") || startsWith(s, "TAR"))
        return RunwaySurface::Asphalt;
    if (startsWith(s, "GRS") || startsWith(s, "GRASS") || startsWith(s, "TURF") || startsWith(s, "GRE") ||
        startsWith(s, "SOD") || s == "G")
        return RunwaySurface::Grass;
    if (startsWith(s, "WAT"))
        return RunwaySurface::Water;
    // Everything else — gravel/dirt/earth/sand/clay/coral/unknown/blank — is an unpaved default.
    return RunwaySurface::Gravel;
}

std::vector<AirportDef> importOurAirports(std::string_view airportsCsv, std::string_view runwaysCsv,
                                          AirportCsvStats* stats) {
    AirportCsvStats st;

    // ── airports.csv → AirportDef keyed by ident ─────────────────────────────
    std::vector<AirportDef> defs;
    std::unordered_map<std::string, std::size_t> byIdent; // ident -> index into defs

    CsvReader areader(airportsCsv);
    std::vector<std::string> row;
    if (!areader.next(row))
        return defs;
    const ColumnMap acols = headerMap(row);
    while (areader.next(row)) {
        const std::string_view type = cell(row, acols, "type");
        if (type == "closed") {
            ++st.skippedClosed;
            continue;
        }
        const std::string ident(cell(row, acols, "ident"));
        double lat = 0.0, lon = 0.0;
        if (ident.empty() || !parseDouble(cell(row, acols, "latitude_deg"), lat) ||
            !parseDouble(cell(row, acols, "longitude_deg"), lon)) {
            ++st.badRows;
            continue;
        }
        AirportDef def;
        def.id = ident;
        def.name = std::string(cell(row, acols, "name"));
        if (def.name.empty())
            def.name = ident;
        def.latRad = lat * kDegToRad<double>;
        def.lonRad = lon * kDegToRad<double>;
        double elevFt = 0.0;
        def.elevationM = parseDouble(cell(row, acols, "elevation_ft"), elevFt) ? elevFt * kMetresPerFoot<double> : -1.0;
        byIdent[ident] = defs.size();
        defs.push_back(std::move(def));
    }

    // ── runways.csv → attach to airports ─────────────────────────────────────
    CsvReader rreader(runwaysCsv);
    if (!rreader.next(row)) {
        st.airports = static_cast<uint32_t>(defs.size());
        if (stats)
            *stats = st;
        return defs;
    }
    const ColumnMap rcols = headerMap(row);
    while (rreader.next(row)) {
        if (cell(row, rcols, "closed") == "1") {
            ++st.skippedClosed;
            continue;
        }
        const std::string aIdent(cell(row, rcols, "airport_ident"));
        const auto it = byIdent.find(aIdent);
        if (it == byIdent.end())
            continue; // runway for a closed/dropped airport
        double lengthFt = 0.0;
        if (!parseDouble(cell(row, rcols, "length_ft"), lengthFt) || lengthFt <= 0.0) {
            ++st.badRows;
            continue;
        }
        double widthFt = 0.0;
        const double widthM = parseDouble(cell(row, rcols, "width_ft"), widthFt) && widthFt > 0.0
                                  ? widthFt * kMetresPerFoot<double>
                                  : 30.0; // sane default
        RunwayDef rw;
        rw.lengthM = static_cast<float>(lengthFt * kMetresPerFoot<double>);
        rw.widthM = static_cast<float>(widthM);
        rw.surface = runwaySurfaceFromOurAirports(cell(row, rcols, "surface"));

        // Heading: le_heading_degT, else endpoint bearing, else the runway-ident number x10, else 0.
        double hdg = 0.0;
        if (!parseDouble(cell(row, rcols, "le_heading_degT"), hdg)) {
            double la0, lo0, la1, lo1;
            if (parseDouble(cell(row, rcols, "le_latitude_deg"), la0) &&
                parseDouble(cell(row, rcols, "le_longitude_deg"), lo0) &&
                parseDouble(cell(row, rcols, "he_latitude_deg"), la1) &&
                parseDouble(cell(row, rcols, "he_longitude_deg"), lo1)) {
                hdg = initialBearingDeg(la0 * kDegToRad<double>, lo0 * kDegToRad<double>, la1 * kDegToRad<double>,
                                        lo1 * kDegToRad<double>);
            } else if (!headingFromIdent(cell(row, rcols, "le_ident"), hdg)) {
                hdg = 0.0;
            }
        }
        rw.headingDeg = static_cast<float>(hdg);

        defs[it->second].runways.push_back(rw);
        ++st.runways;
    }

    st.airports = static_cast<uint32_t>(defs.size());
    if (stats)
        *stats = st;
    return defs;
}

} // namespace fl
