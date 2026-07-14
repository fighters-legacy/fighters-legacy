// SPDX-License-Identifier: GPL-3.0-or-later
//
// fm-trim — derive an aircraft's performance from its flight model, and gate it against the numbers
// its flight manual publishes.
//
// #54's Phase-4 acceptance criterion is "flight model stall speed + fuel burn match design spec for
// each aircraft type". Until this tool there was no way to compute either: nothing in the tree
// derived performance from a FlightModelData. The gate was unmeasurable, and a content author tuning
// an aircraft had no feedback loop shorter than "build the game and fly it".

#include "expect.h"

#include "flight/Trim.h"

#include "flight/FlightModelParser.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace fl;

namespace {

const char* kVersion = "fm-trim 1.0";

void usage() {
    std::printf("Usage: fm-trim <flight-model.toml> [options]\n"
                "\n"
                "Derives performance from a flight model and, with --expect, fails when the model\n"
                "stops reproducing the numbers its flight manual publishes.\n"
                "\n"
                "Options:\n"
                "  --alt a,b,c          Altitudes in metres (default: 0,5000,11000)\n"
                "  --mass <kg>          Gross mass (default: empty + full internal fuel)\n"
                "  --payload <kg>,<cd0> Stores mass and drag, e.g. from an entity's default loadout\n"
                "  --json               Emit machine-readable JSON (consumed by CI and by #821's manual)\n"
                "  --expect <file.toml> Check against published numbers; non-zero exit on a miss\n"
                "  --version            Print version and exit\n"
                "  --help               Print this message and exit\n"
                "\n"
                "Expectation file format:\n"
                "  [[expect]]\n"
                "  metric     = \"stall_speed_1g_mps\"   # or max_level_mach, roc_mps,\n"
                "                                       # sustained_turn_deg_s, instant_turn_deg_s,\n"
                "                                       # corner_speed_mps, sustained_g,\n"
                "                                       # specific_range_m_per_kg\n"
                "  altitude_m = 0\n"
                "  mass_kg    = 6500\n"
                "  expected   = 64.0\n"
                "  tolerance  = 0.05                    # fractional; default 5%%\n");
}

std::vector<float> parseFloatList(const std::string& s) {
    std::vector<float> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ','))
        if (!tok.empty())
            out.push_back(std::strtof(tok.c_str(), nullptr));
    return out;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--version") {
            std::printf("%s\n", kVersion);
            return 0;
        }
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            usage();
            return 0;
        }
    }
    if (argc < 2) {
        usage();
        return 2;
    }

    const std::string modelPath = argv[1];
    std::vector<float> altitudes{0.f, 5000.f, 11000.f};
    float massOverride = 0.f;
    PayloadEffect payload{};
    bool json = false;
    std::string expectPath;

    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--alt" && i + 1 < argc) {
            altitudes = parseFloatList(argv[++i]);
        } else if (a == "--mass" && i + 1 < argc) {
            massOverride = std::strtof(argv[++i], nullptr);
        } else if (a == "--payload" && i + 1 < argc) {
            const auto v = parseFloatList(argv[++i]);
            if (v.size() >= 1)
                payload.extra_mass_kg = v[0];
            if (v.size() >= 2)
                payload.extra_cd0 = v[1];
        } else if (a == "--json") {
            json = true;
        } else if (a == "--expect" && i + 1 < argc) {
            expectPath = argv[++i];
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return 2;
        }
    }

    std::string src;
    if (!readFile(modelPath, src)) {
        std::fprintf(stderr, "cannot read %s\n", modelPath.c_str());
        return 2;
    }

    FlightModelData data;
    try {
        data = parseFlightModel(src);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", modelPath.c_str(), e.what());
        return 2;
    }

    // ── the CI gate ──────────────────────────────────────────────────────────
    if (!expectPath.empty()) {
        std::string expectSrc;
        if (!readFile(expectPath, expectSrc)) {
            std::fprintf(stderr, "cannot read %s\n", expectPath.c_str());
            return 2;
        }
        const ExpectResult r = checkExpectations(data, expectSrc, payload);

        for (const auto& e : r.errors)
            std::fprintf(stderr, "ERROR: %s\n", e.c_str());
        for (const auto& f : r.failures)
            std::fprintf(stderr, "FAIL: %s (%s): expected %.3f +/- %.0f%%, got %.3f\n", f.metric.c_str(),
                         f.detail.c_str(), f.expected, f.tolerance * 100.f, f.actual);

        if (r.ok) {
            std::printf("%s: %d expectation(s) met\n", modelPath.c_str(), r.checked);
            return 0;
        }
        std::fprintf(stderr, "%s: %zu expectation(s) missed\n", modelPath.c_str(), r.failures.size());
        return 1;
    }

    // ── report ───────────────────────────────────────────────────────────────
    const float mass = (massOverride > 0.f) ? massOverride : (data.geometry.mass_kg + data.geometry.fuel_kg);

    std::vector<TrimPoint> points;
    std::vector<TrimResult> results;
    for (float alt : altitudes) {
        TrimPoint pt;
        pt.altitude_m = alt;
        pt.mass_kg = mass;
        points.push_back(pt);
        results.push_back(trim(data, pt, payload));
    }

    if (json) {
        std::printf("%s", toJson(data, points, results).c_str());
        return 0;
    }

    std::printf("%s  (%.0f kg", data.meta.name.c_str(), mass);
    if (payload.extra_mass_kg > 0.f)
        std::printf(" + %.0f kg stores", payload.extra_mass_kg);
    std::printf(")\n\n");

    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        std::printf("  %.0f m\n", points[i].altitude_m);
        if (!r.converged) {
            // Report it; do not guess. An aircraft that cannot hold level flight here has no
            // performance here, and inventing a number would be worse than saying so.
            std::printf("    could not trim — the aircraft cannot hold level flight at this altitude\n\n");
            continue;
        }
        std::printf("    stall speed (1 g)   %7.1f m/s  (%.0f kt)\n", r.stall_speed_1g_mps,
                    r.stall_speed_1g_mps * 1.94384f);
        std::printf("    max level speed     %7.2f M\n", r.max_level_mach);
        std::printf("    rate of climb       %7.1f m/s MIL   %7.1f m/s AB\n", r.roc_mps_mil, r.roc_mps_ab);
        std::printf("    sustained turn      %7.1f deg/s  (%.1f g)\n", r.sustained_turn_deg_s, r.sustained_g);
        std::printf("    instantaneous turn  %7.1f deg/s  (%.1f g)\n", r.instant_turn_deg_s, r.instant_g);
        std::printf("    corner speed        %7.1f m/s  (%.0f kt)\n", r.corner_speed_mps,
                    r.corner_speed_mps * 1.94384f);
        std::printf("    fuel flow           %7.2f kg/s MIL  %7.2f kg/s AB\n", r.fuel_flow_mil_kg_s,
                    r.fuel_flow_ab_kg_s);
        std::printf("    specific range      %7.0f m/kg\n\n", r.specific_range_m_per_kg);
    }

    return 0;
}
