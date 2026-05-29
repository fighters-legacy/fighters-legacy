// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight_model_validator.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

// ── bounds constants ──────────────────────────────────────────────────────────

static constexpr int kClTableAlphaMin = 4;
static constexpr int kClTableMachMin = 2;
static constexpr int kMilThrustMachMin = 2;
static constexpr int kMilThrustAltMin = 2;

static constexpr double kFighterMassMin_kg = 8000.0;
static constexpr double kFighterMassMax_kg = 25000.0;
static constexpr double kFighterWingAreaMin_m2 = 25.0;
static constexpr double kFighterWingAreaMax_m2 = 75.0;
static constexpr double kFighterWingspanMin_m = 8.0;
static constexpr double kFighterWingspanMax_m = 20.0;

// ── valid enum strings ────────────────────────────────────────────────────────

static constexpr const char* kValidAircraftTypes[] = {"fighter",         "interceptor", "attacker", "bomber",
                                                      "maritime_patrol", "awacs",       "ew",       "recon",
                                                      "tanker",          "transport",   "trainer"};
static constexpr std::size_t kValidAircraftTypesCount = sizeof(kValidAircraftTypes) / sizeof(kValidAircraftTypes[0]);

static constexpr const char* kValidEngineTypes[] = {"turbojet", "turbofan", "turboprop", "piston"};
static constexpr std::size_t kValidEngineTypesCount = sizeof(kValidEngineTypes) / sizeof(kValidEngineTypes[0]);

static constexpr const char* kValidHardpointTypes[] = {"missile", "bomb", "fuel", "gun", "pod"};
static constexpr std::size_t kValidHardpointTypesCount = sizeof(kValidHardpointTypes) / sizeof(kValidHardpointTypes[0]);

static constexpr const char* kValidPropRotations[] = {"cw", "ccw", "contra"};
static constexpr std::size_t kValidPropRotationsCount = sizeof(kValidPropRotations) / sizeof(kValidPropRotations[0]);

static constexpr const char* kValidRefuelingTypes[] = {"boom", "drogue"};
static constexpr std::size_t kValidRefuelingTypesCount = sizeof(kValidRefuelingTypes) / sizeof(kValidRefuelingTypes[0]);

static constexpr const char* kValidTankerTypes[] = {"boom", "drogue", "both"};
static constexpr std::size_t kValidTankerTypesCount = sizeof(kValidTankerTypes) / sizeof(kValidTankerTypes[0]);

// ── helpers ───────────────────────────────────────────────────────────────────

static bool isOneOf(const std::string& s, const char* const* valid, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (s == valid[i])
            return true;
    return false;
}

static std::size_t arrayLen(toml::node_view<const toml::node> node) {
    if (auto* arr = node.as_array())
        return arr->size();
    return 0;
}

// ── section validators ────────────────────────────────────────────────────────

static void validateAircraft(const toml::table& tbl, FlightModelValidationResult& r, std::string& outType) {
    auto ac = tbl["aircraft"];
    if (!ac) {
        r.errors.push_back("missing [aircraft] table");
        r.ok = false;
        return;
    }
    if (!ac["name"].value<std::string>()) {
        r.errors.push_back("missing aircraft.name");
        r.ok = false;
    }
    auto type_str = ac["type"].value<std::string>();
    if (!type_str) {
        r.errors.push_back("missing aircraft.type");
        r.ok = false;
    } else if (!isOneOf(*type_str, kValidAircraftTypes, kValidAircraftTypesCount)) {
        r.errors.push_back("aircraft.type: unknown value \"" + *type_str + "\"");
        r.ok = false;
    } else {
        outType = *type_str;
    }
    auto et_str = ac["engine_type"].value<std::string>();
    if (!et_str) {
        r.errors.push_back("missing aircraft.engine_type");
        r.ok = false;
    } else if (!isOneOf(*et_str, kValidEngineTypes, kValidEngineTypesCount)) {
        r.errors.push_back("aircraft.engine_type: unknown value \"" + *et_str + "\"");
        r.ok = false;
    }
    if (!ac["mesh"].value<std::string>()) {
        r.errors.push_back("missing aircraft.mesh");
        r.ok = false;
    }
    if (!ac["cockpit"].value<std::string>()) {
        r.errors.push_back("missing aircraft.cockpit");
        r.ok = false;
    }
}

static void validateFlightModelGeometry(const toml::table& tbl, FlightModelValidationResult& r,
                                        const std::string& aircraftType) {
    auto fm = tbl["flight_model"];
    if (!fm) {
        r.errors.push_back("missing [flight_model] table");
        r.ok = false;
        return;
    }
    auto checkPos = [&](const char* key) {
        auto v = fm[key].value<double>();
        if (!v) {
            r.errors.push_back(std::string("missing flight_model.") + key);
            r.ok = false;
        } else if (*v <= 0.0) {
            r.errors.push_back(std::string("flight_model.") + key + " must be > 0 (got " + std::to_string(*v) + ")");
            r.ok = false;
        }
        return v;
    };
    auto checkNonNeg = [&](const char* key) {
        auto v = fm[key].value<double>();
        if (!v) {
            r.errors.push_back(std::string("missing flight_model.") + key);
            r.ok = false;
        } else if (*v < 0.0) {
            r.errors.push_back(std::string("flight_model.") + key + " must be >= 0 (got " + std::to_string(*v) + ")");
            r.ok = false;
        }
        return v;
    };

    auto mass = checkPos("mass_kg");
    auto wing = checkPos("wing_area_m2");
    auto span = checkPos("wingspan_m");
    checkPos("mac_m");
    checkNonNeg("fuel_kg");
    checkPos("ixx_kg_m2");
    checkPos("iyy_kg_m2");
    checkPos("izz_kg_m2");

    if (aircraftType == "fighter") {
        if (mass && (*mass > 0.0) && (*mass < kFighterMassMin_kg || *mass > kFighterMassMax_kg))
            r.warnings.push_back("flight_model.mass_kg " + std::to_string(*mass) +
                                 " is outside typical fighter range [" + std::to_string(kFighterMassMin_kg) + ", " +
                                 std::to_string(kFighterMassMax_kg) + "]");
        if (wing && (*wing > 0.0) && (*wing < kFighterWingAreaMin_m2 || *wing > kFighterWingAreaMax_m2))
            r.warnings.push_back("flight_model.wing_area_m2 " + std::to_string(*wing) +
                                 " is outside typical fighter range [" + std::to_string(kFighterWingAreaMin_m2) + ", " +
                                 std::to_string(kFighterWingAreaMax_m2) + "]");
        if (span && (*span > 0.0) && (*span < kFighterWingspanMin_m || *span > kFighterWingspanMax_m))
            r.warnings.push_back("flight_model.wingspan_m " + std::to_string(*span) +
                                 " is outside typical fighter range [" + std::to_string(kFighterWingspanMin_m) + ", " +
                                 std::to_string(kFighterWingspanMax_m) + "]");
    }
}

static void validateClTable(const toml::table& tbl, FlightModelValidationResult& r) {
    auto cl = tbl["aero"]["cl_table"];
    if (!cl) {
        r.errors.push_back("missing [aero.cl_table] table");
        r.ok = false;
        return;
    }
    std::size_t alphaLen = arrayLen(cl["alpha"]);
    std::size_t machLen = arrayLen(cl["mach"]);
    if (alphaLen == 0) {
        r.errors.push_back("aero.cl_table.alpha is missing or empty");
        r.ok = false;
    } else if (static_cast<int>(alphaLen) < kClTableAlphaMin) {
        r.errors.push_back("aero.cl_table.alpha must have at least " + std::to_string(kClTableAlphaMin) +
                           " breakpoints (got " + std::to_string(alphaLen) + ")");
        r.ok = false;
    }
    if (machLen == 0) {
        r.errors.push_back("aero.cl_table.mach is missing or empty");
        r.ok = false;
    } else if (static_cast<int>(machLen) < kClTableMachMin) {
        r.errors.push_back("aero.cl_table.mach must have at least " + std::to_string(kClTableMachMin) +
                           " breakpoints (got " + std::to_string(machLen) + ")");
        r.ok = false;
    }
    if (alphaLen > 0 && machLen > 0) {
        std::size_t valLen = arrayLen(cl["values"]);
        std::size_t expected = alphaLen * machLen;
        if (valLen != expected) {
            r.errors.push_back("aero.cl_table.values size mismatch: alpha=" + std::to_string(alphaLen) +
                               " x mach=" + std::to_string(machLen) + " = " + std::to_string(expected) +
                               " expected, got " + std::to_string(valLen));
            r.ok = false;
        }
    }
}

static void validateDragPolar(const toml::table& tbl, FlightModelValidationResult& r) {
    auto dp = tbl["aero"]["drag_polar"];
    if (!dp) {
        r.errors.push_back("missing [aero.drag_polar] table");
        r.ok = false;
        return;
    }
    auto checkPos = [&](const char* key) {
        auto v = dp[key].value<double>();
        if (!v) {
            r.errors.push_back(std::string("missing aero.drag_polar.") + key);
            r.ok = false;
        } else if (*v <= 0.0) {
            r.errors.push_back(std::string("aero.drag_polar.") + key + " must be > 0");
            r.ok = false;
        }
    };
    auto checkNonNeg = [&](const char* key) {
        auto v = dp[key].value<double>();
        if (!v) {
            r.errors.push_back(std::string("missing aero.drag_polar.") + key);
            r.ok = false;
        } else if (*v < 0.0) {
            r.errors.push_back(std::string("aero.drag_polar.") + key + " must be >= 0");
            r.ok = false;
        }
    };
    checkPos("cd0");
    checkPos("k");
    checkNonNeg("speedbrake_cd");
    checkNonNeg("gear_cd");
}

static void validateMoments(const toml::table& tbl, FlightModelValidationResult& r) {
    auto m = tbl["aero"]["moments"];
    if (!m) {
        r.errors.push_back("missing [aero.moments] table");
        r.ok = false;
        return;
    }
    auto checkPresent = [&](const char* key) {
        if (!m[key].value<double>()) {
            r.errors.push_back(std::string("missing aero.moments.") + key);
            r.ok = false;
            return false;
        }
        return true;
    };
    auto checkNeg = [&](const char* key) {
        if (checkPresent(key)) {
            auto v = m[key].value<double>();
            if (v && *v >= 0.0) {
                r.errors.push_back(std::string("aero.moments.") + key + " must be < 0 (got " + std::to_string(*v) +
                                   "); check sign convention in docs/modding/flight-model.md");
                r.ok = false;
            }
        }
    };
    auto checkPos = [&](const char* key) {
        if (checkPresent(key)) {
            auto v = m[key].value<double>();
            if (v && *v <= 0.0) {
                r.errors.push_back(std::string("aero.moments.") + key + " must be > 0 (got " + std::to_string(*v) +
                                   "); check sign convention in docs/modding/flight-model.md");
                r.ok = false;
            }
        }
    };

    checkNeg("cm_alpha");
    checkNeg("cm_q");
    checkPresent("cm_de");
    checkPresent("cl_beta");
    checkNeg("cl_p");
    checkPos("cl_da");
    checkPos("cn_beta");
    checkNeg("cn_r");
    checkPresent("cn_dr");
}

static void validateAeroLimits(const toml::table& tbl, FlightModelValidationResult& r) {
    auto lim = tbl["aero"]["limits"];
    if (!lim) {
        r.errors.push_back("missing [aero.limits] table");
        r.ok = false;
        return;
    }
    auto checkPos = [&](const char* key) {
        auto v = lim[key].value<double>();
        if (!v) {
            r.errors.push_back(std::string("missing aero.limits.") + key);
            r.ok = false;
        } else if (*v <= 0.0) {
            r.errors.push_back(std::string("aero.limits.") + key + " must be > 0");
            r.ok = false;
        }
    };
    checkPos("alpha_stall_deg");
    checkPos("max_g_structural");
    checkPos("max_mach");
    auto minG = lim["min_g_structural"].value<double>();
    if (!minG) {
        r.errors.push_back("missing aero.limits.min_g_structural");
        r.ok = false;
    } else if (*minG >= 0.0) {
        r.errors.push_back("aero.limits.min_g_structural must be < 0 (got " + std::to_string(*minG) + ")");
        r.ok = false;
    }
}

static void validateAeroControls(const toml::table& tbl, FlightModelValidationResult& r) {
    auto ctrl = tbl["aero"]["controls"];
    if (!ctrl) {
        r.errors.push_back("missing [aero.controls] table");
        r.ok = false;
        return;
    }
    for (const char* key : {"max_elevator_deg", "max_aileron_deg", "max_rudder_deg"}) {
        auto v = ctrl[key].value<double>();
        if (!v) {
            r.errors.push_back(std::string("missing aero.controls.") + key);
            r.ok = false;
        } else if (*v <= 0.0) {
            r.errors.push_back(std::string("aero.controls.") + key + " must be > 0");
            r.ok = false;
        }
    }
}

static void validateEngine(const toml::table& tbl, FlightModelValidationResult& r) {
    auto eng = tbl["engine"];
    if (!eng) {
        r.errors.push_back("missing [engine] table");
        r.ok = false;
        return;
    }
    auto checkNonNeg = [&](const char* key) {
        auto v = eng[key].value<double>();
        if (!v) {
            r.errors.push_back(std::string("missing engine.") + key);
            r.ok = false;
        } else if (*v < 0.0) {
            r.errors.push_back(std::string("engine.") + key + " must be >= 0");
            r.ok = false;
        }
    };
    auto checkPos = [&](const char* key) {
        auto v = eng[key].value<double>();
        if (!v) {
            r.errors.push_back(std::string("missing engine.") + key);
            r.ok = false;
        } else if (*v <= 0.0) {
            r.errors.push_back(std::string("engine.") + key + " must be > 0");
            r.ok = false;
        }
    };
    checkNonNeg("fuel_flow_idle_kg_s");
    checkPos("fuel_flow_mil_kg_s");
    checkPos("fuel_flow_ab_kg_s");
    checkNonNeg("spool_time_s");

    auto mil = tbl["engine"]["mil_thrust"];
    if (!mil) {
        r.errors.push_back("missing [engine.mil_thrust] table");
        r.ok = false;
        return;
    }
    std::size_t machLen = arrayLen(mil["mach"]);
    std::size_t altLen = arrayLen(mil["alt_km"]);
    if (machLen == 0) {
        r.errors.push_back("engine.mil_thrust.mach is missing or empty");
        r.ok = false;
    } else if (static_cast<int>(machLen) < kMilThrustMachMin) {
        r.errors.push_back("engine.mil_thrust.mach must have at least " + std::to_string(kMilThrustMachMin) +
                           " breakpoints");
        r.ok = false;
    }
    if (altLen == 0) {
        r.errors.push_back("engine.mil_thrust.alt_km is missing or empty");
        r.ok = false;
    } else if (static_cast<int>(altLen) < kMilThrustAltMin) {
        r.errors.push_back("engine.mil_thrust.alt_km must have at least " + std::to_string(kMilThrustAltMin) +
                           " breakpoints");
        r.ok = false;
    }
    if (machLen > 0 && altLen > 0) {
        std::size_t valLen = arrayLen(mil["values"]);
        std::size_t expected = machLen * altLen;
        if (valLen != expected) {
            r.errors.push_back("engine.mil_thrust.values size mismatch: mach=" + std::to_string(machLen) +
                               " x alt_km=" + std::to_string(altLen) + " = " + std::to_string(expected) +
                               " expected, got " + std::to_string(valLen));
            r.ok = false;
        }
    }
}

static void validateCdWave(const toml::table& tbl, FlightModelValidationResult& r) {
    auto cw = tbl["aero"]["cd_wave"];
    if (!cw)
        return;
    std::size_t machLen = arrayLen(cw["mach"]);
    std::size_t valLen = arrayLen(cw["values"]);
    if (machLen == 0) {
        r.errors.push_back("aero.cd_wave.mach is missing or empty");
        r.ok = false;
    }
    if (valLen == 0) {
        r.errors.push_back("aero.cd_wave.values is missing or empty");
        r.ok = false;
    }
    if (machLen > 0 && valLen > 0 && machLen != valLen) {
        r.errors.push_back("aero.cd_wave: mach and values arrays must have equal length");
        r.ok = false;
    }
}

static void validateAbThrust(const toml::table& tbl, FlightModelValidationResult& r) {
    auto ab = tbl["engine"]["ab_thrust"];
    if (!ab)
        return;
    std::size_t machLen = arrayLen(ab["mach"]);
    std::size_t altLen = arrayLen(ab["alt_km"]);
    if (machLen < static_cast<std::size_t>(kMilThrustMachMin)) {
        r.errors.push_back("engine.ab_thrust.mach must have at least " + std::to_string(kMilThrustMachMin) +
                           " breakpoints");
        r.ok = false;
    }
    if (altLen < static_cast<std::size_t>(kMilThrustAltMin)) {
        r.errors.push_back("engine.ab_thrust.alt_km must have at least " + std::to_string(kMilThrustAltMin) +
                           " breakpoints");
        r.ok = false;
    }
    if (machLen >= static_cast<std::size_t>(kMilThrustMachMin) &&
        altLen >= static_cast<std::size_t>(kMilThrustAltMin)) {
        std::size_t valLen = arrayLen(ab["values"]);
        std::size_t expected = machLen * altLen;
        if (valLen != expected) {
            r.errors.push_back("engine.ab_thrust.values size mismatch: expected " + std::to_string(expected) + " got " +
                               std::to_string(valLen));
            r.ok = false;
        }
    }
}

static void validateTvc(const toml::table& tbl, FlightModelValidationResult& r) {
    auto tvc = tbl["aero"]["tvc"];
    if (!tvc)
        return;
    auto slew = tvc["slew_rate_deg_s"].value<double>();
    if (!slew) {
        r.errors.push_back("missing aero.tvc.slew_rate_deg_s");
        r.ok = false;
    } else if (*slew <= 0.0) {
        r.errors.push_back("aero.tvc.slew_rate_deg_s must be > 0");
        r.ok = false;
    }
    if (!tvc["min_angle_deg"].value<double>()) {
        r.errors.push_back("missing aero.tvc.min_angle_deg");
        r.ok = false;
    }
    if (!tvc["max_angle_deg"].value<double>()) {
        r.errors.push_back("missing aero.tvc.max_angle_deg");
        r.ok = false;
    }
}

static void validateWingSweep(const toml::table& tbl, FlightModelValidationResult& r) {
    auto ws = tbl["wing_sweep"];
    if (!ws)
        return;
    auto refSweep = ws["ref_sweep_deg"].value<double>();
    auto minDeg = ws["min_deg"].value<double>();
    auto maxDeg = ws["max_deg"].value<double>();
    if (!refSweep) {
        r.errors.push_back("missing wing_sweep.ref_sweep_deg");
        r.ok = false;
    }
    if (!minDeg) {
        r.errors.push_back("missing wing_sweep.min_deg");
        r.ok = false;
    }
    if (!maxDeg) {
        r.errors.push_back("missing wing_sweep.max_deg");
        r.ok = false;
    }
    if (refSweep && minDeg && maxDeg) {
        if (*refSweep < *minDeg || *refSweep > *maxDeg) {
            r.errors.push_back("wing_sweep.ref_sweep_deg must be within [min_deg, max_deg]");
            r.ok = false;
        }
    }
    if (!ws["slew_rate_deg_s"].value<double>()) {
        r.errors.push_back("missing wing_sweep.slew_rate_deg_s");
        r.ok = false;
    }
}

static void validateProp(const toml::table& tbl, FlightModelValidationResult& r) {
    auto p = tbl["prop"];
    if (!p)
        return;
    auto rot = p["rotation"].value<std::string>();
    if (!rot) {
        r.errors.push_back("missing prop.rotation");
        r.ok = false;
    } else if (!isOneOf(*rot, kValidPropRotations, kValidPropRotationsCount)) {
        r.errors.push_back("prop.rotation: unknown value \"" + *rot + "\"");
        r.ok = false;
    }
    for (const char* key : {"torque_factor", "gyro_factor"}) {
        if (!p[key].value<double>()) {
            r.errors.push_back(std::string("missing prop.") + key);
            r.ok = false;
        }
    }
}

static void validateCarrier(const toml::table& tbl, FlightModelValidationResult& r) {
    auto c = tbl["carrier"];
    if (!c)
        return;
    for (const char* key : {"approach_m_s", "cat_min_m_s", "hook_length_m"}) {
        auto v = c[key].value<double>();
        if (!v) {
            r.errors.push_back(std::string("missing carrier.") + key);
            r.ok = false;
        } else if (*v <= 0.0) {
            r.errors.push_back(std::string("carrier.") + key + " must be > 0");
            r.ok = false;
        }
    }
    if (!c["approach_aoa_deg"].value<double>()) {
        r.errors.push_back("missing carrier.approach_aoa_deg");
        r.ok = false;
    }
}

static void validateRefueling(const toml::table& tbl, FlightModelValidationResult& r) {
    auto ref = tbl["refueling"];
    if (!ref)
        return;
    auto type_str = ref["type"].value<std::string>();
    if (!type_str) {
        r.errors.push_back("missing refueling.type");
        r.ok = false;
    } else if (!isOneOf(*type_str, kValidRefuelingTypes, kValidRefuelingTypesCount)) {
        r.errors.push_back("refueling.type: unknown value \"" + *type_str + "\"");
        r.ok = false;
    }
    auto rate = ref["max_rate_kg_s"].value<double>();
    if (!rate) {
        r.errors.push_back("missing refueling.max_rate_kg_s");
        r.ok = false;
    } else if (*rate <= 0.0) {
        r.errors.push_back("refueling.max_rate_kg_s must be > 0");
        r.ok = false;
    }
}

static void validateTanker(const toml::table& tbl, FlightModelValidationResult& r) {
    auto t = tbl["tanker"];
    if (!t)
        return;
    auto type_str = t["type"].value<std::string>();
    if (!type_str) {
        r.errors.push_back("missing tanker.type");
        r.ok = false;
    } else if (!isOneOf(*type_str, kValidTankerTypes, kValidTankerTypesCount)) {
        r.errors.push_back("tanker.type: unknown value \"" + *type_str + "\"");
        r.ok = false;
    }
    auto rate = t["max_rate_kg_s"].value<double>();
    if (!rate) {
        r.errors.push_back("missing tanker.max_rate_kg_s");
        r.ok = false;
    } else if (*rate <= 0.0) {
        r.errors.push_back("tanker.max_rate_kg_s must be > 0");
        r.ok = false;
    }
    if (!t["offload_reserve"].value<double>()) {
        r.errors.push_back("missing tanker.offload_reserve");
        r.ok = false;
    }
}

static void validateHardpoints(const toml::table& tbl, FlightModelValidationResult& r) {
    auto* hp_arr = tbl["hardpoints"].as_array();
    if (!hp_arr || hp_arr->empty())
        return;

    std::set<int64_t> seenSlots;
    std::size_t idx = 0;
    for (auto& el : *hp_arr) {
        auto* hp = el.as_table();
        if (!hp) {
            r.errors.push_back("hardpoints[" + std::to_string(idx) + "] is not a table");
            r.ok = false;
            ++idx;
            continue;
        }
        auto slot_v = (*hp)["slot"].value<int64_t>();
        if (!slot_v) {
            r.errors.push_back("hardpoints[" + std::to_string(idx) + "] missing slot");
            r.ok = false;
        } else if (*slot_v < 0) {
            r.errors.push_back("hardpoints[" + std::to_string(idx) + "].slot must be >= 0");
            r.ok = false;
        } else if (!seenSlots.insert(*slot_v).second) {
            r.errors.push_back("hardpoints[" + std::to_string(idx) + "].slot " + std::to_string(*slot_v) +
                               " is duplicated");
            r.ok = false;
        }
        auto type_str = (*hp)["type"].value<std::string>();
        if (!type_str) {
            r.errors.push_back("hardpoints[" + std::to_string(idx) + "] missing type");
            r.ok = false;
        } else if (!isOneOf(*type_str, kValidHardpointTypes, kValidHardpointTypesCount)) {
            r.errors.push_back("hardpoints[" + std::to_string(idx) + "].type: unknown value \"" + *type_str + "\"");
            r.ok = false;
        }
        auto* allowed = (*hp)["allowed"].as_array();
        if (!allowed || allowed->empty()) {
            r.errors.push_back("hardpoints[" + std::to_string(idx) + "].allowed must be a non-empty array");
            r.ok = false;
        }
        auto default_v = (*hp)["default"].value<std::string>();
        if (!default_v) {
            r.errors.push_back("hardpoints[" + std::to_string(idx) + "] missing default");
            r.ok = false;
        } else if (allowed && !allowed->empty()) {
            bool found = false;
            for (auto& a : *allowed) {
                if (a.value<std::string>() == *default_v) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                r.errors.push_back("hardpoints[" + std::to_string(idx) + "].default \"" + *default_v +
                                   "\" is not in allowed list");
                r.ok = false;
            }
        }
        ++idx;
    }
}

// ── public entry point ────────────────────────────────────────────────────────

FlightModelValidationResult validateFlightModel(std::string_view tomlContent) {
    FlightModelValidationResult r;

    toml::table tbl;
    try {
        tbl = toml::parse(tomlContent);
    } catch (const toml::parse_error& e) {
        r.errors.push_back(std::string("TOML parse error: ") + e.what());
        r.ok = false;
        return r;
    }

    std::string aircraftType;
    validateAircraft(tbl, r, aircraftType);
    validateFlightModelGeometry(tbl, r, aircraftType);
    validateClTable(tbl, r);
    validateDragPolar(tbl, r);
    validateMoments(tbl, r);
    validateAeroLimits(tbl, r);
    validateAeroControls(tbl, r);
    validateEngine(tbl, r);
    validateCdWave(tbl, r);
    validateAbThrust(tbl, r);
    validateTvc(tbl, r);
    validateWingSweep(tbl, r);
    validateProp(tbl, r);
    validateCarrier(tbl, r);
    validateRefueling(tbl, r);
    validateTanker(tbl, r);
    validateHardpoints(tbl, r);

    return r;
}
