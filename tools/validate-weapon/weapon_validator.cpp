// SPDX-License-Identifier: GPL-3.0-or-later
#include "weapon_validator.h"

#include "weapon/WeaponDef.h"
#include "weapon/WeaponDefParser.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace fl {

namespace {

// Plausibility bounds. These are NOT schema limits — everything here parses, flies, and is allowed.
// They mark values so far outside real ordnance that they are more likely a typo or a unit mix-up
// than an intent (the classic being metres typed into a field that wants feet).
constexpr float kImplausibleMaxG = 100.f;
constexpr float kImplausibleBlastRadiusM = 300.f; // ~1000 ft
constexpr float kImplausibleMassKg = 5000.f;      // ~11 000 lb
constexpr float kImplausibleDragFactor = 0.2f;    // a store draggier than the whole airframe
constexpr float kImplausibleSpeedMps = 2000.f;    // ~Mach 6
constexpr float kTinyMassKg = 5.f;                // ~11 lb

void checkPlausibility(const WeaponDef& w, WeaponValidationResult& r) {
    if (w.performance.maxG > kImplausibleMaxG)
        r.warnings.push_back("performance.max_g is implausibly high (" + std::to_string(w.performance.maxG) + " g)");

    if (w.warhead.blastRadiusM > kImplausibleBlastRadiusM)
        r.warnings.push_back("warhead.blast_radius_ft is implausibly large — check the units");

    if (w.performance.maxSpeedMps > kImplausibleSpeedMps)
        r.warnings.push_back("performance.max_speed_kts is implausibly high — check the units");

    if (w.load.massKg > kImplausibleMassKg)
        r.warnings.push_back("load.weight_lb is implausibly heavy for an air-carried store");
    if (w.load.massKg < kTinyMassKg)
        r.warnings.push_back("load.weight_lb is implausibly light for an air-carried store");

    if (w.load.dragFactor > kImplausibleDragFactor)
        r.warnings.push_back("load.drag_factor is implausibly high (a single store draggier than a "
                             "clean airframe)");

    // A guided weapon that cannot see as far as it can shoot is usually a mistake: it can be
    // launched at a target it will never acquire. Legal, and occasionally intended (a shot taken on
    // a datalink handoff), so it warns rather than fails.
    if (w.seeker && w.seeker->acquisitionRangeM > 0.f && w.seeker->acquisitionRangeM < w.performance.maxRangeM * 0.25f)
        r.warnings.push_back("seeker.acquisition_nm is far below the weapon's max range — shots "
                             "beyond acquisition range will never take a lock");

    // A powered weapon with no motor burn, or an unpowered one with a long burn, is a category slip.
    if (w.type == WeaponType::Missile && w.performance.motorBurnTimeS <= 0.f)
        r.warnings.push_back("a missile with no performance.motor_burn_time_s is unpowered");
    if (w.type == WeaponType::Bomb && w.performance.motorBurnTimeS > 0.f)
        r.warnings.push_back("a bomb with a performance.motor_burn_time_s is powered — should its "
                             "type be missile or rocket?");
}

[[nodiscard]] std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Collects the weapon ids a pack defines. Uses the real parser, so a malformed weapon is reported
// as such rather than quietly contributing no id (which would then surface as a confusing
// "unknown weapon" against every entity that references it).
void collectWeaponIds(const fs::path& weaponsDir, std::set<std::string>& ids, WeaponValidationResult& r) {
    if (!fs::is_directory(weaponsDir))
        return;

    for (const auto& entry : fs::directory_iterator(weaponsDir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".toml")
            continue;

        const std::string src = readFile(entry.path());
        try {
            const WeaponDef w = parseWeaponDef(src);
            if (!ids.insert(w.id).second) {
                r.errors.push_back("duplicate weapon id \"" + w.id + "\" (" + entry.path().filename().string() + ")");
                r.ok = false;
            }
        } catch (const std::exception& e) {
            r.errors.push_back(entry.path().filename().string() + ": " + e.what());
            r.ok = false;
        }
    }
}

// Pulls the weapon ids an entity's hardpoints reference. Deliberately reads only the id STRINGS via
// toml++ rather than going through parseEntityDef: the hardpoint schema (slot uniqueness, default ∈
// allowed, known type) is already the parser's job and is covered by its tests. Re-checking it here
// would be exactly the duplicate-vocabulary drift this tool exists to prevent.
void collectHardpointRefs(const fs::path& entitiesDir, std::vector<std::pair<std::string, std::string>>& refs,
                          WeaponValidationResult& r) {
    if (!fs::is_directory(entitiesDir))
        return;

    for (const auto& entry : fs::directory_iterator(entitiesDir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".toml")
            continue;

        toml::table tbl;
        try {
            tbl = toml::parse(readFile(entry.path()));
        } catch (const toml::parse_error& e) {
            r.errors.push_back(entry.path().filename().string() +
                               ": TOML parse error: " + std::string(e.description()));
            r.ok = false;
            continue;
        }

        auto* arr = tbl["hardpoints"].as_array();
        if (!arr)
            continue;

        const std::string file = entry.path().filename().string();
        for (auto& el : *arr) {
            auto* hp = el.as_table();
            if (!hp)
                continue;
            if (auto* allowed = (*hp)["allowed"].as_array()) {
                for (auto& a : *allowed) {
                    if (auto id = a.value<std::string>(); id && !id->empty())
                        refs.emplace_back(file, *id);
                }
            }
            if (auto def = (*hp)["default"].value<std::string>(); def && !def->empty())
                refs.emplace_back(file, *def);
        }
    }
}

} // namespace

WeaponValidationResult validateWeapon(std::string_view tomlContent) {
    WeaponValidationResult r;

    WeaponDef w;
    try {
        w = parseWeaponDef(tomlContent);
    } catch (const std::exception& e) {
        r.errors.push_back(e.what());
        r.ok = false;
        return r; // nothing parsed, so there is nothing to judge the plausibility of
    }

    checkPlausibility(w, r);
    return r;
}

WeaponValidationResult validatePackLoadouts(const std::string& packDir) {
    WeaponValidationResult r;

    const fs::path root(packDir);
    if (!fs::is_directory(root)) {
        r.errors.push_back("not a directory: " + packDir);
        r.ok = false;
        return r;
    }

    std::set<std::string> weaponIds;
    collectWeaponIds(root / "weapons", weaponIds, r);

    std::vector<std::pair<std::string, std::string>> refs;
    collectHardpointRefs(root / "entities", refs, r);

    for (const auto& [file, id] : refs) {
        if (weaponIds.find(id) == weaponIds.end()) {
            r.errors.push_back(file + ": hardpoint references unknown weapon id \"" + id + "\"");
            r.ok = false;
        }
    }

    return r;
}

} // namespace fl
