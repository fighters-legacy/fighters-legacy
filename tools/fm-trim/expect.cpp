// SPDX-License-Identifier: GPL-3.0-or-later
#include "expect.h"

#include <toml++/toml.hpp>

#include <cmath>
#include <sstream>
#include <string>

namespace fl {

void checkMaxMach(const FlightModelData& d, ExpectResult& out, const PayloadEffect& payload) {
    // At the tropopause, where a fighter is fastest.
    TrimPoint pt;
    pt.altitude_m = 11000.f;
    pt.mass_kg = d.geometry.mass_kg + d.geometry.fuel_kg;
    const TrimResult r = trim(d, pt, payload);
    if (!r.converged)
        return;

    constexpr float kMaxMachSlack = 1.05f; // 5%
    if (d.limits.max_mach > 0.f && r.max_level_mach > d.limits.max_mach * kMaxMachSlack) {
        std::ostringstream os;
        os << "model reaches Mach " << r.max_level_mach << " in level flight at 11 km but declares max_mach "
           << d.limits.max_mach
           << ". A model that can outrun its own declared limit is a broken model, not a fast "
              "aircraft -- fix cd_wave or the thrust deck.";
        out.errors.push_back(os.str());
        out.ok = false;
    }
}

ExpectResult checkExpectations(const FlightModelData& d, std::string_view expectToml, const PayloadEffect& payload) {
    ExpectResult out;

    toml::table tbl;
    try {
        tbl = toml::parse(expectToml);
    } catch (const toml::parse_error& e) {
        out.errors.push_back(std::string("expectation file parse error: ") + e.what());
        out.ok = false;
        return out;
    }

    auto* arr = tbl["expect"].as_array();
    if (!arr || arr->empty()) {
        out.errors.push_back("expectation file has no [[expect]] entries");
        out.ok = false;
        return out;
    }

    for (auto& node : *arr) {
        auto* e = node.as_table();
        if (!e)
            continue;

        ExpectEntry ex;
        ex.metric = (*e)["metric"].value_or(std::string{});
        ex.altitude_m = static_cast<float>((*e)["altitude_m"].value_or(0.0));
        ex.mass_kg = static_cast<float>((*e)["mass_kg"].value_or(0.0));
        ex.expected = static_cast<float>((*e)["expected"].value_or(0.0));
        ex.tolerance = static_cast<float>((*e)["tolerance"].value_or(0.05));
        ex.afterburner = (*e)["afterburner"].value_or(true);
        ex.mach = static_cast<float>((*e)["mach"].value_or(0.0));
        ex.load_factor = static_cast<float>((*e)["load_factor"].value_or(0.0));
        ex.payload_kg = static_cast<float>((*e)["payload_kg"].value_or(0.0));
        ex.payload_cd0 = static_cast<float>((*e)["payload_cd0"].value_or(0.0));

        if (ex.metric.empty()) {
            out.errors.push_back("an [[expect]] entry has no `metric`");
            out.ok = false;
            continue;
        }

        TrimPoint pt;
        pt.altitude_m = ex.altitude_m;
        pt.mass_kg = ex.mass_kg;
        pt.mach = ex.mach;
        pt.load_factor = ex.load_factor;
        pt.afterburner = ex.afterburner;

        // A row's own stores override the CLI-wide payload, so one file can gate the clean AND the
        // loaded condition -- which is what checks the store-drag path at all (#826).
        PayloadEffect rowPayload = payload;
        if (ex.payload_kg > 0.f || ex.payload_cd0 > 0.f)
            rowPayload = PayloadEffect{ex.payload_kg, ex.payload_cd0};

        const TrimResult r = trim(d, pt, rowPayload);

        if (!r.converged) {
            out.errors.push_back("could not trim '" + ex.metric + "' at " + std::to_string(ex.altitude_m) +
                                 " m: the model cannot hold level flight there");
            out.ok = false;
            continue;
        }

        float actual = 0.f;
        if (ex.metric == "stall_speed_1g_mps")
            actual = r.stall_speed_1g_mps;
        else if (ex.metric == "min_level_speed_mps")
            actual = r.min_level_speed_mps;
        else if (ex.metric == "max_level_mach")
            actual = r.max_level_mach;
        else if (ex.metric == "roc_mps")
            actual = ex.afterburner ? r.roc_mps_ab : r.roc_mps_mil;
        else if (ex.metric == "sustained_turn_deg_s")
            actual = r.sustained_turn_deg_s;
        else if (ex.metric == "instant_turn_deg_s")
            actual = r.instant_turn_deg_s;
        else if (ex.metric == "corner_speed_mps")
            actual = r.corner_speed_mps;
        else if (ex.metric == "sustained_g")
            actual = r.sustained_g;
        else if (ex.metric == "specific_range_m_per_kg")
            actual = r.specific_range_m_per_kg;
        else if (ex.metric == "max_lift_g" || ex.metric == "ps_mps") {
            // These only exist AT a pinned Mach; without one there is no condition to evaluate them at,
            // and silently returning 0 would look like a failing model rather than a malformed row.
            if (ex.mach <= 0.f) {
                out.errors.push_back("metric '" + ex.metric + "' requires a `mach` on the row");
                out.ok = false;
                continue;
            }
            if (ex.metric == "ps_mps" && ex.load_factor <= 0.f) {
                out.errors.push_back("metric 'ps_mps' requires a `load_factor` on the row");
                out.ok = false;
                continue;
            }
            actual = (ex.metric == "max_lift_g") ? r.max_lift_g : r.ps_mps;
        } else {
            out.errors.push_back("unknown metric '" + ex.metric + "'");
            out.ok = false;
            continue;
        }

        ++out.checked;
        // ps_mps is signed and passes through zero (that IS the sustained condition), so a fractional
        // tolerance is meaningless on it -- 5% of 0 is 0. Treat its tolerance as ABSOLUTE, in m/s.
        const float allowed = (ex.metric == "ps_mps") ? ex.tolerance : std::abs(ex.expected) * ex.tolerance;
        if (std::abs(actual - ex.expected) > allowed) {
            ExpectFailure f;
            f.metric = ex.metric;
            f.expected = ex.expected;
            f.actual = actual;
            f.tolerance = ex.tolerance;
            std::ostringstream os;
            os << "at " << ex.altitude_m << " m, " << ex.mass_kg << " kg";
            f.detail = os.str();
            out.failures.push_back(std::move(f));
            out.ok = false;
        }
    }

    checkMaxMach(d, out, payload);
    return out;
}

std::string toJson(const FlightModelData& d, const std::vector<TrimPoint>& points,
                   const std::vector<TrimResult>& results) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(3);

    os << "{\n";
    os << "  \"aircraft\": \"" << d.meta.name << "\",\n";
    os << "  \"limits\": {\n";
    os << "    \"alpha_stall_deg\": " << d.limits.alpha_stall_deg << ",\n";
    os << "    \"max_g_structural\": " << d.limits.max_g_structural << ",\n";
    os << "    \"min_g_structural\": " << d.limits.min_g_structural << ",\n";
    os << "    \"max_mach\": " << d.limits.max_mach << "\n";
    os << "  },\n";
    os << "  \"points\": [\n";

    for (std::size_t i = 0; i < results.size() && i < points.size(); ++i) {
        const auto& p = points[i];
        const auto& r = results[i];
        os << "    {\n";
        os << "      \"altitude_m\": " << p.altitude_m << ",\n";
        os << "      \"mass_kg\": " << p.mass_kg << ",\n";
        os << "      \"converged\": " << (r.converged ? "true" : "false") << ",\n";
        os << "      \"stall_speed_1g_mps\": " << r.stall_speed_1g_mps << ",\n";
        os << "      \"min_level_speed_mps\": " << r.min_level_speed_mps << ",\n";
        os << "      \"max_level_mach\": " << r.max_level_mach << ",\n";
        os << "      \"roc_mps_mil\": " << r.roc_mps_mil << ",\n";
        os << "      \"roc_mps_ab\": " << r.roc_mps_ab << ",\n";
        os << "      \"sustained_turn_deg_s\": " << r.sustained_turn_deg_s << ",\n";
        os << "      \"sustained_g\": " << r.sustained_g << ",\n";
        os << "      \"instant_turn_deg_s\": " << r.instant_turn_deg_s << ",\n";
        os << "      \"instant_g\": " << r.instant_g << ",\n";
        os << "      \"corner_speed_mps\": " << r.corner_speed_mps << ",\n";
        os << "      \"fuel_flow_mil_kg_s\": " << r.fuel_flow_mil_kg_s << ",\n";
        os << "      \"fuel_flow_ab_kg_s\": " << r.fuel_flow_ab_kg_s << ",\n";
        os << "      \"specific_range_m_per_kg\": " << r.specific_range_m_per_kg << "\n";
        os << "    }" << (i + 1 < results.size() ? "," : "") << "\n";
    }

    os << "  ]\n";
    os << "}\n";
    return os.str();
}

} // namespace fl
