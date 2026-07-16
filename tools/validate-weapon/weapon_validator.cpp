// SPDX-License-Identifier: GPL-3.0-or-later
#include "weapon_validator.h"

#include "ILogger.h"
#include "content/AssetManager.h"
#include "content/ContentIndex.h"
#include "content/FolderContentPack.h"
#include "sensor/BuiltinSensors.h"
#include "sensor/SensorDef.h"
#include "sensor/SensorDefParser.h"
#include "weapon/WeaponDef.h"
#include "weapon/WeaponDefParser.h"

#include <stdfs/StdFilesystem.h>

#include <array>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>

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

    // The pre-#583 ad-hoc seeker lobe still parses, but the seeker head is a sensor def now
    // (2026-07-14 decision record) and the legacy form is removed after one release.
    if (w.seeker && w.seeker->usesLegacyLobe())
        r.warnings.push_back("seeker uses the deprecated fov_deg/acquisition_nm lobe — reference a "
                             "sensor def with sensor_id");

    // A guided weapon that cannot see as far as it can shoot is usually a mistake: it can be
    // launched at a target it will never acquire. Legal, and occasionally intended (a shot taken on
    // a datalink handoff), so it warns rather than fails. In sensor_id form the lobe lives on the
    // sensor def, so the equivalent check runs in --pack mode where the def can be resolved.
    if (w.seeker && w.seeker->usesLegacyLobe() && w.seeker->acquisitionRangeM > 0.f &&
        w.seeker->acquisitionRangeM < w.performance.maxRangeM * 0.25f)
        r.warnings.push_back("seeker.acquisition_nm is far below the weapon's max range — shots "
                             "beyond acquisition range will never take a lock");

    // A powered weapon with no motor burn, or an unpowered one with a long burn, is a category slip.
    if (w.type == WeaponType::Missile && w.performance.motorBurnTimeS <= 0.f)
        r.warnings.push_back("a missile with no performance.motor_burn_time_s is unpowered");
    if (w.type == WeaponType::Bomb && w.performance.motorBurnTimeS > 0.f)
        r.warnings.push_back("a bomb with a performance.motor_burn_time_s is powered — should its "
                             "type be missile or rocket?");
}

} // namespace

WeaponValidationResult checkWeaponPlausibility(const WeaponDef& w) {
    WeaponValidationResult r;
    checkPlausibility(w, r);
    return r;
}

namespace {

[[nodiscard]] std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
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

namespace {

// The content system's own log lines would duplicate the findings this tool reports itself.
class SilentLoggerW final : public ILogger {
  public:
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

} // namespace

WeaponValidationResult validatePackWeapons(const std::string& packDir) {
    WeaponValidationResult r;

    const fs::path root(packDir);
    if (!fs::is_directory(root)) {
        r.errors.push_back("not a directory: " + packDir);
        r.ok = false;
        return r;
    }

    const fs::path weaponsDir = root / "weapons";
    if (!fs::is_directory(weaponsDir))
        return r; // a pack with no weapons has nothing to validate — not an error

    // A seeker's sensor_id is a def-id reference into the pack's sensors/ — resolved through the
    // REAL id index (the same rule validate-entity follows: never a private copy of the path or
    // id rules). The manifest is irrelevant to id resolution, so a synthesized one is fine.
    static SilentLoggerW silent;
    StdFilesystem stdfs(root, root);
    FolderContentPack::Manifest manifest;
    manifest.id = "pack-under-validation";
    manifest.name = manifest.id;
    manifest.namespaceId = manifest.id;
    auto folderPack = std::make_unique<FolderContentPack>(stdfs, silent, ".", manifest);
    FolderContentPack* pack = folderPack.get();
    pack->init();
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(folderPack));
    AssetManager assets(std::move(packs), silent);
    ContentIndex index;
    constexpr std::array<AssetType, 1> kDefTypes{AssetType::SensorDef};
    index.build(assets, kDefTypes, silent);

    std::set<std::string> ids;
    for (const auto& entry : fs::directory_iterator(weaponsDir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".toml")
            continue;

        const std::string file = entry.path().filename().string();
        const std::string src = readFile(entry.path());

        WeaponDef w;
        try {
            w = parseWeaponDef(src);
        } catch (const std::exception& e) {
            r.errors.push_back(file + ": " + e.what());
            r.ok = false;
            continue;
        }

        if (!ids.insert(w.id).second) {
            r.errors.push_back(file + ": duplicate weapon id \"" + w.id + "\"");
            r.ok = false;
        }

        WeaponValidationResult one;
        checkPlausibility(w, one);
        for (auto& warning : one.warnings)
            r.warnings.push_back(file + ": " + warning);

        // Resolve the seeker head. Unresolvable is an ERROR for the same reason as an entity's
        // flight_model: the runtime consequence is a missile that flies but never sees.
        if (w.seeker && !w.seeker->sensorId.empty() && w.seeker->sensorId != sensor::BuiltinSensors::eyeball().id) {
            const std::string* assetName = index.assetNameFor(AssetType::SensorDef, w.seeker->sensorId);
            if (assetName == nullptr) {
                r.errors.push_back(file + ": seeker.sensor_id \"" + w.seeker->sensorId +
                                   "\" does not resolve to any sensor def in this pack");
                r.ok = false;
            } else if (auto raw = pack->loadSensorDef(assetName->c_str())) {
                // The lobe now lives on the sensor def, so the "can it see as far as it shoots"
                // plausibility check runs here, against the real lobe.
                try {
                    const sensor::SensorDef sd = sensor::parseSensorDef(
                        std::string_view(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size()));
                    if (sd.search.maxRangeM > 0.f && sd.search.maxRangeM < w.performance.maxRangeM * 0.25f)
                        r.warnings.push_back(file + ": the seeker's search lobe reaches far less than "
                                                    "the weapon's max range — shots beyond it will "
                                                    "never take a lock");
                } catch (const std::exception&) {
                    // A malformed sensor def is validate-sensor's finding, not this tool's.
                }
            }
        }
    }

    return r;
}

} // namespace fl
