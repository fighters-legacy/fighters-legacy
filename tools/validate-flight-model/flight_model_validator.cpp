// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight_model_validator.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

// ── bounds constants ──────────────────────────────────────────────────────────

static constexpr int kClTableAlphaMin = 4;
static constexpr int kClTableMachMin = 2;
static constexpr int kMilThrustMachMin = 2;
static constexpr int kMilThrustAltMin = 2;

// The alpha at which cl_table peaks must agree with aero.limits.alpha_stall_deg to within this many
// degrees (#816). Breakpoints are coarse, so the tolerance is one typical table step, not zero.
static constexpr double kStallPeakToleranceDeg = 2.0;

// ── plausibility bands ────────────────────────────────────────────────────────
//
// TWO KINDS OF CHECK, AND THEY ARE NOT THE SAME THING (#815).
//
// The ABSOLUTE bands below are wide, and deliberately so. An absolute band can only ever catch a
// TYPO or a UNIT ERROR -- a mass entered in pounds, a wing area in square feet. It has no business
// encoding a design philosophy. The old bands (mass [8000, 25000], area [25, 75]) were calibrated on
// the F-15/F-16/F-18 class and excluded the entire light-fighter class: an honest F-5E Tiger II
// (4349 kg, 17.28 m^2, 8.13 m) tripped two of the three, and so would a MiG-21, a Gnat or a Tejas.
//
// The RATIO bands are the ones that actually mean something, because they are class-independent:
// wing loading and aspect ratio are what make an aeroplane a fighter, and the F-5E, the F-15C and
// the F-16 all sit comfortably inside them despite a 3x spread in mass.
//
// We deliberately did NOT add a `light_fighter` aircraft type. That would push a validator's problem
// into the content schema and make every pack author pick a bucket to satisfy a lint. The content
// describes the aircraft; it does not describe the validator's taxonomy.

static constexpr double kMassMin_kg = 3000.0;   // below this, someone has typo'd
static constexpr double kMassMax_kg = 40000.0;  // above this, it is not a fighter-class airframe
static constexpr double kWingAreaMin_m2 = 12.0; // F-5E is 17.28
static constexpr double kWingAreaMax_m2 = 90.0;
static constexpr double kWingspanMin_m = 6.0; // F-5E is 8.13
static constexpr double kWingspanMax_m = 24.0;

// Wing loading (kg/m^2): F-5E 252, F-15C 225, F-16 ~330, T-38A 207.
static constexpr double kFighterWingLoadingMin = 120.0;
static constexpr double kFighterWingLoadingMax = 600.0;
static constexpr double kTrainerWingLoadingMin = 100.0;
static constexpr double kTrainerWingLoadingMax = 400.0;

// Aspect ratio (b^2/S): F-5E 3.83, F-15C 3.01, F-16 3.2, T-38A 3.76.
static constexpr double kFighterAspectRatioMin = 1.5;
static constexpr double kFighterAspectRatioMax = 5.0;
static constexpr double kTrainerAspectRatioMin = 2.5;
static constexpr double kTrainerAspectRatioMax = 6.0;

// ── one band set per airframe CLASS (#1182) ───────────────────────────────────
//
// The bands above describe a fighter, and the early return that used to sit below them meant every
// other role -- bomber, transport, tanker, awacs, ew, maritime_patrol -- was checked against
// NOTHING. fl-base-pack's B-1B validated clean while none of its numbers were range-checked, and
// would have validated equally clean with its mass in pounds.
//
// Applying the fighter bands to a bomber instead would be worse: the B-1B legitimately sits outside
// every one of them (87 t against a 40 t ceiling, 181 m^2 against 90, 41.8 m of span against 24,
// 481 kg/m^2 of wing loading, aspect ratio 9.6 against 5.0). So large aircraft get their own set,
// with the same philosophy as the originals -- wide enough that only a unit error or a typo trips
// them, and cross-checked against real airframes rather than invented:
//
//   empty mass    B-52 83 t, C-17 128 t, C-5 172 t, KC-135 45 t, C-130 34 t, B-1B 87 t
//   wing area     C-130 162, B-1B 181, KC-135 226, B-52 370, C-5 576 m^2
//   span          KC-135 39.9, B-1B 41.8 (spread), B-52 56.4, C-5 67.9 m
//   wing loading  KC-135 198, B-52 225, C-17 363, B-1B 481 kg/m^2   (on EMPTY mass, as computed here)
//   aspect ratio  KC-135 7.0, C-17 7.6, B-52 8.6, B-1B 9.6 (spread)
//
// A variable-sweep wing reports the aspect ratio of whichever configuration `wingspan_m` describes,
// which is the spread planform and therefore the high end -- hence the headroom above 9.6.
struct BandSet {
    const char* name;
    double massMin, massMax;
    double areaMin, areaMax;
    double spanMin, spanMax;
    double wingLoadingMin, wingLoadingMax;
    double arMin, arMax;
};

static constexpr BandSet kFighterBands{"fighter",
                                       kMassMin_kg,
                                       kMassMax_kg,
                                       kWingAreaMin_m2,
                                       kWingAreaMax_m2,
                                       kWingspanMin_m,
                                       kWingspanMax_m,
                                       kFighterWingLoadingMin,
                                       kFighterWingLoadingMax,
                                       kFighterAspectRatioMin,
                                       kFighterAspectRatioMax};

static constexpr BandSet kTrainerBands{"trainer",
                                       kMassMin_kg,
                                       kMassMax_kg,
                                       kWingAreaMin_m2,
                                       kWingAreaMax_m2,
                                       kWingspanMin_m,
                                       kWingspanMax_m,
                                       kTrainerWingLoadingMin,
                                       kTrainerWingLoadingMax,
                                       kTrainerAspectRatioMin,
                                       kTrainerAspectRatioMax};

static constexpr BandSet kLargeBands{"large-aircraft", 20000.0, 450000.0, 60.0, 700.0, 25.0, 80.0, 150.0,
                                     1000.0,           5.0,     12.0};

// `recon` deliberately has NO band set: the role spans an RF-5 through a U-2 to a Global Hawk, and
// no single honest range covers them. It takes the "not checked" note instead, which is the truthful
// answer rather than a band nobody could justify.
static const BandSet* bandsForRole(std::string_view type) {
    if (type == "fighter" || type == "interceptor" || type == "attacker")
        return &kFighterBands;
    if (type == "trainer")
        return &kTrainerBands;
    if (type == "bomber" || type == "transport" || type == "tanker" || type == "awacs" || type == "ew" ||
        type == "maritime_patrol")
        return &kLargeBands;
    return nullptr;
}

// ── valid enum strings ────────────────────────────────────────────────────────

static constexpr const char* kValidAircraftTypes[] = {"fighter",         "interceptor", "attacker", "bomber",
                                                      "maritime_patrol", "awacs",       "ew",       "recon",
                                                      "tanker",          "transport",   "trainer"};
static constexpr std::size_t kValidAircraftTypesCount = sizeof(kValidAircraftTypes) / sizeof(kValidAircraftTypes[0]);

static constexpr const char* kValidEngineTypes[] = {"turbojet", "turbofan", "turboprop", "piston"};
static constexpr std::size_t kValidEngineTypesCount = sizeof(kValidEngineTypes) / sizeof(kValidEngineTypes[0]);

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
    // aircraft.mesh / aircraft.cockpit are NOT required (#813): a flight model is aerodynamics and
    // does not know what it looks like. Asset wiring belongs to the entity def (entity.mesh /
    // entity.cockpit), which is where the renderer has always read it from.
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
    auto ixx = checkPos("ixx_kg_m2");
    checkPos("iyy_kg_m2");
    auto izz = checkPos("izz_kg_m2");

    // Optional product of inertia (#899). A physically valid inertia tensor needs Ixz² < Ixx·Izz
    // (else the coupled roll/yaw solve has a non-positive determinant); flag a transcription error.
    if (auto ixz = fm["ixz_kg_m2"].value<double>(); ixz && ixx && izz && *ixx > 0.0 && *izz > 0.0) {
        if ((*ixz) * (*ixz) >= (*ixx) * (*izz)) {
            r.errors.push_back("flight_model.ixz_kg_m2 " + std::to_string(*ixz) +
                               " is too large: Ixz^2 must be < Ixx*Izz for a valid inertia tensor");
            r.ok = false;
        }
    }

    const BandSet* bands = bandsForRole(aircraftType);
    if (bands == nullptr) {
        // A validator that cannot check something should SAY it did not check, rather than pass in
        // silence (#1182). Silence is what let fl-base-pack's B-1B validate clean while nothing at
        // all was range-checked -- it would have validated equally clean with its mass in pounds.
        r.warnings.push_back("aircraft.type \"" + aircraftType +
                             "\" has no plausibility band set, so mass, wing area, span, wing loading "
                             "and aspect ratio were NOT range-checked");
        return;
    }

    auto band = [&](const char* field, double value, double lo, double hi, const std::string& what) {
        if (value < lo || value > hi)
            r.warnings.push_back(std::string(field) + " " + std::to_string(value) + " is outside the plausible " +
                                 what + " range [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
    };

    // Absolutes: unit-error and typo detection only (see the note on the constants).
    if (mass && *mass > 0.0)
        band("flight_model.mass_kg", *mass, bands->massMin, bands->massMax, "airframe");
    if (wing && *wing > 0.0)
        band("flight_model.wing_area_m2", *wing, bands->areaMin, bands->areaMax, "airframe");
    if (span && *span > 0.0)
        band("flight_model.wingspan_m", *span, bands->spanMin, bands->spanMax, "airframe");

    // Ratios: the checks that actually describe an aeroplane, and the reason the F-5E and the F-15C
    // can both pass despite a 3x difference in mass.
    if (mass && wing && *mass > 0.0 && *wing > 0.0) {
        const double wingLoading = *mass / *wing;
        band("wing loading (mass_kg / wing_area_m2)", wingLoading, bands->wingLoadingMin, bands->wingLoadingMax,
             std::string(bands->name) + " wing-loading");
    }
    if (span && wing && *span > 0.0 && *wing > 0.0) {
        const double aspectRatio = (*span * *span) / *wing;
        band("aspect ratio (wingspan_m^2 / wing_area_m2)", aspectRatio, bands->arMin, bands->arMax,
             std::string(bands->name) + " aspect-ratio");
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

// THE CHECK THAT MAKES alpha_stall_deg LOAD-BEARING (#816).
//
// The engine deliberately does NOT clamp CL at alpha_stall_deg -- the cl_table already carries the
// post-stall collapse if the author wrote an honest one, and clamping on top would double-count the
// stall and reward an author who did not. But that only works if the two agree. A model whose lift
// peaks at 25 deg while declaring it stalls at 18 is lying about itself, and every consumer of the
// flag -- buffet, HUD, AI, and #54's "stall speed matches design spec" gate -- inherits the lie.
//
// So: the alpha at which cl_table peaks must be within kStallPeakToleranceDeg of alpha_stall_deg.
static void validateStallConsistency(const toml::table& tbl, FlightModelValidationResult& r) {
    auto cl = tbl["aero"]["cl_table"];
    auto stallNode = tbl["aero"]["limits"]["alpha_stall_deg"].value<double>();
    if (!cl || !stallNode)
        return; // missing sections are reported by their own validators

    auto* alphaArr = cl["alpha"].as_array();
    auto* valArr = cl["values"].as_array();
    if (!alphaArr || !valArr || alphaArr->empty())
        return;

    const std::size_t alphaLen = alphaArr->size();
    const std::size_t machLen = arrayLen(cl["mach"]);
    if (machLen == 0 || valArr->size() != alphaLen * machLen)
        return; // shape errors are reported by validateClTable

    // Peak CL over the whole table: the highest lift the aircraft can make, at whichever alpha it
    // happens at. Row-major, so row i (alpha i) spans [i*machLen, (i+1)*machLen).
    double peakCl = -1e30;
    double peakAlpha = 0.0;
    for (std::size_t i = 0; i < alphaLen; ++i) {
        for (std::size_t j = 0; j < machLen; ++j) {
            auto v = valArr->get(i * machLen + j)->value<double>();
            if (v && *v > peakCl) {
                peakCl = *v;
                auto a = alphaArr->get(i)->value<double>();
                peakAlpha = a ? *a : 0.0;
            }
        }
    }

    const double declared = *stallNode;
    if (std::abs(peakAlpha - declared) > kStallPeakToleranceDeg) {
        r.errors.push_back("aero.cl_table peaks at alpha " + std::to_string(peakAlpha) +
                           " deg but aero.limits.alpha_stall_deg declares " + std::to_string(declared) +
                           " deg. The engine does not clamp CL at the stall -- the table IS the stall -- so the two "
                           "must agree within " +
                           std::to_string(kStallPeakToleranceDeg) + " deg or the model is lying about itself.");
        r.ok = false;
    }
}

// Optional tabulated total clean drag (#820). Same shape rules as cl_table.
static void validateCdTable(const toml::table& tbl, FlightModelValidationResult& r) {
    auto cd = tbl["aero"]["cd_table"];
    if (!cd)
        return; // optional -- the parabolic drag_polar remains the simple path

    std::size_t alphaLen = arrayLen(cd["alpha"]);
    std::size_t machLen = arrayLen(cd["mach"]);
    if (static_cast<int>(alphaLen) < kClTableAlphaMin) {
        r.errors.push_back("aero.cd_table.alpha must have at least " + std::to_string(kClTableAlphaMin) +
                           " breakpoints (got " + std::to_string(alphaLen) + ")");
        r.ok = false;
    }
    if (static_cast<int>(machLen) < kClTableMachMin) {
        r.errors.push_back("aero.cd_table.mach must have at least " + std::to_string(kClTableMachMin) +
                           " breakpoints (got " + std::to_string(machLen) + ")");
        r.ok = false;
    }

    std::size_t valLen = arrayLen(cd["values"]);
    if (alphaLen > 0 && machLen > 0) {
        std::size_t expected = alphaLen * machLen;
        if (valLen != expected) {
            r.errors.push_back("aero.cd_table.values size mismatch: alpha=" + std::to_string(alphaLen) +
                               " x mach=" + std::to_string(machLen) + " = " + std::to_string(expected) +
                               " expected, got " + std::to_string(valLen));
            r.ok = false;
        }
    }

    // Drag is strictly positive. A zero or negative CD is a transcription error, and it would make
    // the aircraft accelerate under its own drag.
    if (auto* arr = cd["values"].as_array()) {
        for (std::size_t i = 0; i < arr->size(); ++i) {
            auto v = arr->get(i)->value<double>();
            if (v && *v <= 0.0) {
                r.errors.push_back("aero.cd_table.values[" + std::to_string(i) + "] must be > 0 (got " +
                                   std::to_string(*v) + ")");
                r.ok = false;
                break;
            }
        }
    }

    // THE DOUBLE-COUNT GUARD. A cd_table is TOTAL clean drag -- it already includes the induced term.
    // Authoring a non-zero drag_polar.k alongside it means the author believes one of the two is
    // additive, and computeForces would silently apply roughly twice the drag they intended. That is
    // exactly the class of bug a content author cannot debug from the outside, so it is an error, not
    // a warning. Set k = 0.0 to say "the table owns the drag".
    auto k = tbl["aero"]["drag_polar"]["k"].value<double>();
    if (k && *k > 0.0) {
        r.errors.push_back("aero.cd_table and a non-zero aero.drag_polar.k are both authored: the table is TOTAL "
                           "clean drag and already includes induced drag, so k would double-count it. Set "
                           "drag_polar.k = 0.0 when using cd_table.");
        r.ok = false;
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
    // k must be positive on the parabolic path -- an aircraft with no induced drag is not an
    // aircraft. But when a cd_table owns the drag (#820), k = 0.0 is exactly how the author declares
    // that, and validateCdTable errors if it is anything else.
    if (tbl["aero"]["cd_table"])
        checkNonNeg("k");
    else
        checkPos("k");
    checkNonNeg("speedbrake_cd");
    checkNonNeg("gear_cd");
}

// [articulation] + [aero.flaps] (#842). Both optional with defaults, so a model that omits them is
// valid; what the validator catches is a transit time that would make an actuator behave absurdly and
// a flap that reduces parasite drag.
static void validateArticulation(const toml::table& tbl, FlightModelValidationResult& r) {
    if (auto art = tbl["articulation"]; art && art.as_table()) {
        static const char* kKeys[] = {"gear_transit_s", "flap_transit_s", "speedbrake_transit_s", "hook_transit_s",
                                      "canopy_transit_s"};
        for (const char* key : kKeys) {
            auto v = art[key].value<double>();
            if (!v)
                continue;
            if (!(*v >= 0.0) || *v > 600.0) {
                r.errors.push_back(std::string("articulation.") + key +
                                   " must be in [0, 600] seconds (0 = instantaneous)");
                r.ok = false;
            } else if (*v > 0.0 && *v < 0.1) {
                r.warnings.push_back(std::string("articulation.") + key + " = " + std::to_string(*v) +
                                     " s is implausibly fast for a real actuator");
            }
        }
    }
    if (auto fl_ = tbl["aero"]["flaps"]; fl_ && fl_.as_table()) {
        if (auto dcd = fl_["dcd"].value<double>(); dcd && *dcd < 0.0) {
            r.errors.push_back("aero.flaps.dcd must be >= 0 (a flap cannot reduce parasite drag)");
            r.ok = false;
        }
        if (auto dcl = fl_["dcl"].value<double>(); dcl && *dcl < 0.0)
            r.warnings.push_back("aero.flaps.dcl is negative — a flap that REDUCES lift is unusual");
    }
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

    // Optional alpha-dependent dampers (#899): a Table1D over alpha. When present it replaces the
    // scalar, so the arrays must be equal-length with at least 2 breakpoints.
    auto checkDamperTable = [&](const char* key) {
        auto sub = m[key];
        if (!sub || !sub.as_table())
            return;
        std::size_t nAlpha = arrayLen(sub["alpha"]);
        std::size_t nVal = arrayLen(sub["values"]);
        if (nAlpha < 2) {
            r.errors.push_back(std::string("aero.moments.") + key + ".alpha must have at least 2 breakpoints");
            r.ok = false;
        }
        if (nAlpha != nVal) {
            r.errors.push_back(std::string("aero.moments.") + key + ".alpha (" + std::to_string(nAlpha) +
                               ") and values (" + std::to_string(nVal) + ") must be equal length");
            r.ok = false;
        }
    };
    checkDamperTable("cm_q_table");
    checkDamperTable("cl_p_table");
    checkDamperTable("cn_r_table");
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

    // Optional FLCS AoA cap (#900). It must be a positive angle, and it is a CAP below the aerodynamic
    // stall — a cap at or above alpha_stall_deg can never bind and almost certainly means the two were
    // confused (the whole point is that the FLCS holds the jet short of the aero peak).
    if (auto cap = lim["alpha_limit_deg"].value<double>()) {
        if (*cap <= 0.0) {
            r.errors.push_back("aero.limits.alpha_limit_deg must be > 0 (got " + std::to_string(*cap) + ")");
            r.ok = false;
        } else if (auto stall = lim["alpha_stall_deg"].value<double>(); stall && *cap >= *stall) {
            r.warnings.push_back("aero.limits.alpha_limit_deg " + std::to_string(*cap) + " is >= alpha_stall_deg " +
                                 std::to_string(*stall) +
                                 "; the FLCS cap should sit below the aerodynamic stall or it never binds");
        }
    }

    // Optional dynamic-pressure placard in KEAS (#1181). Real placards run from a light trainer's
    // ~300 kn to the F-5E's and T-38A's 710 kn; the band is wide because it can only ever catch a
    // unit error — knots confused with m/s, or a Mach number typed into a knots field.
    if (auto keas = lim["max_keas"].value<double>()) {
        if (*keas <= 0.0) {
            r.errors.push_back("aero.limits.max_keas must be > 0 when present (got " + std::to_string(*keas) +
                               "); omit the key entirely for an aircraft with no placard");
            r.ok = false;
        } else if (*keas < 100.0 || *keas > 900.0) {
            r.warnings.push_back("aero.limits.max_keas " + std::to_string(*keas) +
                                 " is outside the plausible placard range [100, 900] knots EAS; it is KNOTS "
                                 "equivalent airspeed, not m/s and not a Mach number");
        }
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

    // Optional asymmetric pitch travel (#822). It is a TRAVEL, so it is a positive magnitude — the
    // sign is carried by the stick, not by the number. Authoring it negative would silently reverse
    // the aircraft's nose-down response, which is the kind of bug that gets blamed on the sim.
    if (auto neg = ctrl["max_elevator_neg_deg"]) {
        auto v = neg.value<double>();
        if (!v || *v <= 0.0) {
            r.errors.push_back("aero.controls.max_elevator_neg_deg must be > 0 (it is a travel magnitude; "
                               "nose-down is expressed by the stick, not by a negative number)");
            r.ok = false;
        } else if (auto pos = ctrl["max_elevator_deg"].value<double>(); pos && *v > *pos) {
            // Not an error -- a canard or a tailless design could plausibly have more nose-down than
            // nose-up travel -- but on a conventional fighter it is almost always a transcription
            // slip, and it doubles the aircraft's bunt authority if it goes unnoticed.
            r.warnings.push_back("aero.controls.max_elevator_neg_deg " + std::to_string(*v) +
                                 " exceeds max_elevator_deg " + std::to_string(*pos) +
                                 " — unusual: most aircraft have more nose-up authority than nose-down");
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

    // Optional engine-out asymmetry params (#308): only checked when present.
    if (auto n = eng["engine_count"]) {
        if (!n.is_integer() || n.as_integer()->get() < 1) {
            r.errors.push_back("engine.engine_count must be an integer >= 1");
            r.ok = false;
        }
    }
    if (auto a = eng["engine_yaw_arm_frac"].value<double>(); a && *a < 0.0) {
        r.errors.push_back("engine.engine_yaw_arm_frac must be >= 0");
        r.ok = false;
    }

    // Optional afterburner envelope (#309): range-check when present; warn if set without an AB deck
    // (the limit would never apply).
    const bool hasAbDeck = static_cast<bool>(tbl["engine"]["ab_thrust"].as_table());
    if (auto m = eng["ab_min_mach"].value<double>()) {
        if (*m < 0.0) {
            r.errors.push_back("engine.ab_min_mach must be >= 0");
            r.ok = false;
        } else if (!hasAbDeck) {
            r.warnings.push_back("engine.ab_min_mach is set but there is no [engine.ab_thrust] deck; "
                                 "the afterburner limit will never apply");
        }
    }
    if (auto a = eng["ab_max_alt_km"].value<double>()) {
        if (*a <= 0.0) {
            r.errors.push_back("engine.ab_max_alt_km must be > 0");
            r.ok = false;
        } else if (!hasAbDeck) {
            r.warnings.push_back("engine.ab_max_alt_km is set but there is no [engine.ab_thrust] deck; "
                                 "the afterburner limit will never apply");
        }
    }

    // Optional engine failure dynamics (#308): range-check when present, plus the cross-field
    // sanity that the combustion ceiling sits ABOVE the afterburner ceiling — an engine that quits
    // entirely below the altitude where its augmentor still lights is a units mistake.
    if (auto f = eng["flameout_alt_km"].value<double>()) {
        if (*f <= 0.0) {
            r.errors.push_back("engine.flameout_alt_km must be > 0");
            r.ok = false;
        } else {
            if (*f < 5.0 || *f > 40.0)
                r.warnings.push_back("engine.flameout_alt_km outside the plausible 5-40 km band; "
                                     "check units (kilometres, not metres or feet)");
            if (auto a = eng["ab_max_alt_km"].value<double>(); a && *f <= *a)
                r.warnings.push_back("engine.flameout_alt_km is at or below engine.ab_max_alt_km; the whole "
                                     "engine would flame out before the afterburner envelope limit applies");
        }
    }
    if (auto rl = eng["relight_min_mps"].value<double>(); rl && (*rl < 0.0 || *rl > 300.0)) {
        r.errors.push_back("engine.relight_min_mps must be in [0, 300]");
        r.ok = false;
    }
    if (auto sm = eng["surge_alpha_margin_deg"].value<double>()) {
        if (*sm < 0.0 || *sm > 45.0) {
            r.errors.push_back("engine.surge_alpha_margin_deg must be in [0, 45]");
            r.ok = false;
        }
        if (!eng["compressor_stall"].value<bool>().value_or(false))
            r.warnings.push_back("engine.surge_alpha_margin_deg is set but engine.compressor_stall is not "
                                 "enabled; the surge model will never run");
    }

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

// Shared shape check for the optional (mach, alt_km) thrust decks — ab_thrust and idle_thrust (#898)
// have identical structure, so one helper keeps their rules from drifting.
static void validateOptionalThrustTable(const toml::table& tbl, const char* key, FlightModelValidationResult& r) {
    auto t = tbl["engine"][key];
    if (!t)
        return;
    const std::string prefix = std::string("engine.") + key;
    std::size_t machLen = arrayLen(t["mach"]);
    std::size_t altLen = arrayLen(t["alt_km"]);
    if (machLen < static_cast<std::size_t>(kMilThrustMachMin)) {
        r.errors.push_back(prefix + ".mach must have at least " + std::to_string(kMilThrustMachMin) + " breakpoints");
        r.ok = false;
    }
    if (altLen < static_cast<std::size_t>(kMilThrustAltMin)) {
        r.errors.push_back(prefix + ".alt_km must have at least " + std::to_string(kMilThrustAltMin) + " breakpoints");
        r.ok = false;
    }
    if (machLen >= static_cast<std::size_t>(kMilThrustMachMin) &&
        altLen >= static_cast<std::size_t>(kMilThrustAltMin)) {
        std::size_t valLen = arrayLen(t["values"]);
        std::size_t expected = machLen * altLen;
        if (valLen != expected) {
            r.errors.push_back(prefix + ".values size mismatch: expected " + std::to_string(expected) + " got " +
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

    // Hardpoints moved to the entity definition TOML in #623: weapon stations are a property of the
    // entity, not of its aerodynamics. Fail loudly rather than ignoring them, so a pack that still
    // declares them here learns why its stations stopped existing instead of silently flying clean.
    r.errors.push_back("[[hardpoints]] is no longer part of the flight model: weapon stations moved to the entity "
                       "definition TOML (#623). Move the [[hardpoints]] blocks to the entity's .toml unchanged.");
    r.ok = false;
}

// ── public entry point ────────────────────────────────────────────────────────

// [drone_limits] (#351): the UAV autopilot command envelope. Range checks, the min<max airspeed
// cross-check, and the "your autopilot limit is looser than the airframe" warning (a max_g above
// max_g_structural never binds — the airframe breaks first).
static void validateDroneLimits(const toml::table& tbl, FlightModelValidationResult& r) {
    auto dl = tbl["drone_limits"];
    if (!dl)
        return;
    auto nonNeg = [&](const char* key) {
        auto v = dl[key].value<double>();
        if (v && *v < 0.0) {
            r.errors.push_back(std::string("drone_limits.") + key + " must be >= 0 (0 = that gate off)");
            r.ok = false;
        }
        return v;
    };
    if (auto bank = nonNeg("max_bank_deg"); bank && *bank > 89.0) {
        r.errors.push_back("drone_limits.max_bank_deg must be <= 89");
        r.ok = false;
    }
    const auto g = nonNeg("max_g");
    const auto vmin = nonNeg("min_airspeed_mps");
    const auto vmax = nonNeg("max_airspeed_mps");
    if (vmin && vmax && *vmin > 0.0 && *vmax > 0.0 && *vmin >= *vmax) {
        r.errors.push_back("drone_limits.min_airspeed_mps must be below max_airspeed_mps");
        r.ok = false;
    }
    if (g && *g > 0.0) {
        if (auto structural = tbl["aero"]["limits"]["max_g_structural"].value<double>();
            structural && *g >= *structural)
            r.warnings.push_back("drone_limits.max_g is at or above max_g_structural; the autopilot limit "
                                 "never binds — the airframe breaks first");
    }
}

// Rotorcraft core (#349/#350): mass, fuel and inertias are required; wing geometry is NOT (a rotor
// model carries its own reference areas), so this deliberately does not reuse
// validateFlightModelGeometry, whose wing fields are hard requirements.
static void validateRotorcraftCore(const toml::table& tbl, FlightModelValidationResult& r) {
    if (!tbl["aircraft"]["name"].value<std::string>()) {
        r.errors.push_back("missing aircraft.name");
        r.ok = false;
    }
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
    checkPos("mass_kg");
    checkPos("ixx_kg_m2");
    checkPos("iyy_kg_m2");
    checkPos("izz_kg_m2");
    if (auto fuel = fm["fuel_kg"].value<double>(); !fuel) {
        r.errors.push_back("missing flight_model.fuel_kg");
        r.ok = false;
    } else if (*fuel < 0.0) {
        r.errors.push_back("flight_model.fuel_kg must be >= 0");
        r.ok = false;
    }
}

// [multirotor] (#349): required rotor fields positive, plus the check that actually matters — a
// frame whose total max thrust cannot lift its own weight can never hover, which is a units error
// nine times out of ten.
static void validateMultirotor(const toml::table& tbl, FlightModelValidationResult& r) {
    auto mr = tbl["multirotor"];
    if (!mr) {
        r.errors.push_back("multirotor model: missing [multirotor] table");
        r.ok = false;
        return;
    }
    const auto count = mr["rotor_count"];
    if (!count.is_integer() || count.as_integer()->get() < 3 || count.as_integer()->get() > 16) {
        r.errors.push_back("multirotor.rotor_count must be an integer in [3, 16]");
        r.ok = false;
    }
    auto checkPos = [&](const char* key) {
        auto v = mr[key].value<double>();
        if (!v) {
            r.errors.push_back(std::string("missing multirotor.") + key);
            r.ok = false;
        } else if (*v <= 0.0) {
            r.errors.push_back(std::string("multirotor.") + key + " must be > 0");
            r.ok = false;
        }
        return v;
    };
    const auto thrust = checkPos("rotor_thrust_max_n");
    checkPos("rotor_arm_m");
    checkPos("yaw_torque_nm");
    if (auto ft = mr["flight_time_min"].value<double>(); ft && *ft <= 0.0) {
        r.errors.push_back("multirotor.flight_time_min must be > 0");
        r.ok = false;
    }

    // Hover check: N * T_max must clear the all-up weight with margin, or the vehicle cannot hover
    // (let alone climb or hold attitude, which spends part of the same thrust budget).
    const auto mass = tbl["flight_model"]["mass_kg"].value<double>();
    const auto fuel = tbl["flight_model"]["fuel_kg"].value<double>();
    if (thrust && mass && count.is_integer()) {
        const double totalThrust = static_cast<double>(count.as_integer()->get()) * *thrust;
        const double weight = (*mass + fuel.value_or(0.0)) * 9.80665;
        if (totalThrust <= weight) {
            r.errors.push_back("multirotor cannot hover: rotor_count * rotor_thrust_max_n (" +
                               std::to_string(totalThrust) + " N) does not exceed the all-up weight (" +
                               std::to_string(weight) + " N)");
            r.ok = false;
        } else if (totalThrust < 1.3 * weight) {
            r.warnings.push_back("multirotor thrust-to-weight is under 1.3; expect sluggish climb and "
                                 "attitude authority");
        }
    }
}

// [helicopter] (#350): required disc/control fields positive, the cannot-hover error, and a disc
// loading plausibility band — real helicopters run roughly 100–900 N/m^2 of disc; far outside that
// is a units mistake (radius in feet, thrust in kgf...).
static void validateHelicopter(const toml::table& tbl, FlightModelValidationResult& r) {
    auto h = tbl["helicopter"];
    if (!h) {
        r.errors.push_back("helicopter model: missing [helicopter] table");
        r.ok = false;
        return;
    }
    auto checkPos = [&](const char* key) {
        auto v = h[key].value<double>();
        if (!v) {
            r.errors.push_back(std::string("missing helicopter.") + key);
            r.ok = false;
        } else if (*v <= 0.0) {
            r.errors.push_back(std::string("helicopter.") + key + " must be > 0");
            r.ok = false;
        }
        return v;
    };
    const auto radius = checkPos("main_rotor_radius_m");
    const auto thrust = checkPos("main_rotor_max_thrust_n");
    checkPos("yaw_moment_max_nm");
    checkPos("cyclic_moment_nm");

    // Turboshaft fuel flows are required (the runtime parser requires them too).
    auto eng = tbl["engine"];
    if (!eng) {
        r.errors.push_back("helicopter model: missing [engine] table (turboshaft fuel flows)");
        r.ok = false;
    } else {
        for (const char* key : {"fuel_flow_idle_kg_s", "fuel_flow_mil_kg_s"}) {
            if (!eng[key].value<double>()) {
                r.errors.push_back(std::string("missing engine.") + key);
                r.ok = false;
            }
        }
    }

    const auto mass = tbl["flight_model"]["mass_kg"].value<double>();
    const auto fuel = tbl["flight_model"]["fuel_kg"].value<double>();
    if (thrust && mass) {
        const double weight = (*mass + fuel.value_or(0.0)) * 9.80665;
        if (*thrust <= weight) {
            r.errors.push_back("helicopter cannot hover: main_rotor_max_thrust_n (" + std::to_string(*thrust) +
                               " N) does not exceed the all-up weight (" + std::to_string(weight) + " N)");
            r.ok = false;
        } else if (*thrust < 1.15 * weight) {
            r.warnings.push_back("helicopter thrust-to-weight is under 1.15; expect a marginal hover with no "
                                 "climb reserve");
        }
    }
    if (thrust && radius && *radius > 0.0) {
        const double discLoading = *thrust / (3.14159265358979 * *radius * *radius);
        if (discLoading < 100.0 || discLoading > 900.0)
            r.warnings.push_back("helicopter disc loading " + std::to_string(discLoading) +
                                 " N/m^2 is outside the plausible 100-900 band; check units");
    }
}

// [vessel] (#38): required propulsion fields positive, plus a top-speed plausibility band — a
// displacement hull past 25 m/s (~50 kt) is a units mistake, not a warship.
static void validateVessel(const toml::table& tbl, FlightModelValidationResult& r) {
    auto v = tbl["vessel"];
    if (!v) {
        r.errors.push_back("vessel model: missing [vessel] table");
        r.ok = false;
        return;
    }
    auto checkPos = [&](const char* key) {
        auto val = v[key].value<double>();
        if (!val) {
            r.errors.push_back(std::string("missing vessel.") + key);
            r.ok = false;
        } else if (*val <= 0.0) {
            r.errors.push_back(std::string("vessel.") + key + " must be > 0");
            r.ok = false;
        }
        return val;
    };
    checkPos("max_thrust_n");
    if (auto spd = checkPos("max_speed_mps"); spd && *spd > 25.0)
        r.warnings.push_back("vessel.max_speed_mps over 25 (~50 kt) is implausible for a displacement hull; "
                             "check units (m/s, not knots)");
    if (auto tr = v["turn_rate_deg_s"].value<double>(); tr && (*tr <= 0.0 || *tr > 10.0)) {
        r.errors.push_back("vessel.turn_rate_deg_s must be in (0, 10]");
        r.ok = false;
    }
}

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

    // Ballistic vehicles (#354) validate against the reduced schema the runtime parser accepts:
    // [aircraft] name/type, [flight_model] masses, and [engine.boost]. Requiring CL tables and
    // turbine fuel flows of an unwinged booster would force authors to invent numbers nothing
    // reads (BallisticForceModel flies thrust + drag only).
    if (const auto typeStr = tbl["aircraft"]["type"].value<std::string>(); typeStr && *typeStr == "ballistic") {
        if (!tbl["aircraft"]["name"].value<std::string>()) {
            r.errors.push_back("missing aircraft.name");
            r.ok = false;
        }
        validateFlightModelGeometry(tbl, r, "ballistic");
        auto boost = tbl["engine"]["boost"];
        if (!boost) {
            r.errors.push_back("ballistic model: missing [engine.boost] table");
            r.ok = false;
            return r;
        }
        const auto thrust = boost["thrust_n"].value<double>();
        const auto burn = boost["burn_time_s"].value<double>();
        if (!thrust || *thrust <= 0.0) {
            r.errors.push_back("engine.boost.thrust_n must be a number > 0");
            r.ok = false;
        }
        if (!burn || *burn <= 0.0) {
            r.errors.push_back("engine.boost.burn_time_s must be a number > 0");
            r.ok = false;
        }
        return r;
    }

    // Multirotors (#349) validate against the reduced rotor schema the runtime parser accepts —
    // same rationale as ballistic: CL tables and turbine decks would be invented numbers.
    if (const auto typeStr = tbl["aircraft"]["type"].value<std::string>(); typeStr && *typeStr == "multirotor") {
        validateRotorcraftCore(tbl, r);
        validateMultirotor(tbl, r);
        return r;
    }

    // Helicopters (#350): the rotor-disc reduced schema.
    if (const auto typeStr = tbl["aircraft"]["type"].value<std::string>(); typeStr && *typeStr == "helicopter") {
        validateRotorcraftCore(tbl, r);
        validateHelicopter(tbl, r);
        return r;
    }

    // Surface vessels (#38): the ship-shaped reduced schema.
    if (const auto typeStr = tbl["aircraft"]["type"].value<std::string>(); typeStr && *typeStr == "vessel") {
        validateRotorcraftCore(tbl, r); // masses + inertias; wing geometry not required — same as rotorcraft
        validateVessel(tbl, r);
        return r;
    }

    validateAircraft(tbl, r, aircraftType);
    validateFlightModelGeometry(tbl, r, aircraftType);
    validateClTable(tbl, r);
    validateStallConsistency(tbl, r);
    validateCdTable(tbl, r);
    validateDragPolar(tbl, r);
    validateArticulation(tbl, r);
    validateMoments(tbl, r);
    validateAeroLimits(tbl, r);
    validateAeroControls(tbl, r);
    validateEngine(tbl, r);
    validateCdWave(tbl, r);
    validateOptionalThrustTable(tbl, "ab_thrust", r);
    validateOptionalThrustTable(tbl, "idle_thrust", r);
    validateTvc(tbl, r);
    validateWingSweep(tbl, r);
    validateProp(tbl, r);
    validateCarrier(tbl, r);
    validateRefueling(tbl, r);
    validateTanker(tbl, r);
    validateHardpoints(tbl, r);
    validateDroneLimits(tbl, r);

    return r;
}

} // namespace fl
