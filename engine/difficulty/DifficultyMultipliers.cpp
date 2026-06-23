// SPDX-License-Identifier: GPL-3.0-or-later
#include "difficulty/DifficultyMultipliers.h"

#include "IFilesystem.h"
#include "ILogger.h"
#include "content/AssetManager.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#include <toml++/toml.hpp>

namespace fl {

// ---------------------------------------------------------------------------
// Hardcoded defaults (values from the issue spec table)
// ---------------------------------------------------------------------------

static PresetValues hardcodedCadet() {
    PresetValues v;
    v.flightAssists = FlightAssists::AllOn;
    v.aimAssist = true;
    v.enemyLabels = EnemyLabels::Always;
    v.radarRealism = RadarRealism::Simple;
    v.blackoutRedout = false;
    v.fuelConsumption = false;
    v.inFlightRefueling = RefuelingMode::Auto;
    v.friendlyFire = false;
    v.crashDamage = false;
    v.rearmMode = RearmMode::Instantaneous;
    v.reactionTimeS = 1.5f;
    v.aimErrorDeg = 8.0f;
    v.radarSensorRange = 0.50f;
    v.countermeasureUse = CountermeasureUse::Never;
    v.energyManagement = EnergyManagement::Passive;
    v.samEngagementRange = 0.60f;
    v.samRadarShutdown = SamRadarShutdown::Never;
    return v;
}

static PresetValues hardcodedPilot() {
    PresetValues v;
    v.flightAssists = FlightAssists::GLimiterOnly;
    v.aimAssist = true;
    v.enemyLabels = EnemyLabels::OnLock;
    v.radarRealism = RadarRealism::Standard;
    v.blackoutRedout = true;
    v.fuelConsumption = true;
    v.inFlightRefueling = RefuelingMode::Simplified;
    v.friendlyFire = false;
    v.crashDamage = true;
    v.rearmMode = RearmMode::Timed;
    v.reactionTimeS = 0.8f;
    v.aimErrorDeg = 4.0f;
    v.radarSensorRange = 0.80f;
    v.countermeasureUse = CountermeasureUse::Reactive;
    v.energyManagement = EnergyManagement::Standard;
    v.samEngagementRange = 0.80f;
    v.samRadarShutdown = SamRadarShutdown::Sometimes;
    return v;
}

static PresetValues hardcodedAce() {
    PresetValues v;
    v.flightAssists = FlightAssists::AllOff;
    v.aimAssist = false;
    v.enemyLabels = EnemyLabels::Off;
    v.radarRealism = RadarRealism::Full;
    v.blackoutRedout = true;
    v.fuelConsumption = true;
    v.inFlightRefueling = RefuelingMode::Manual;
    v.friendlyFire = true;
    v.crashDamage = true;
    v.rearmMode = RearmMode::SupplyLimited;
    v.reactionTimeS = 0.3f;
    v.aimErrorDeg = 1.0f;
    v.radarSensorRange = 1.00f;
    v.countermeasureUse = CountermeasureUse::Proactive;
    v.energyManagement = EnergyManagement::AggressiveBfm;
    v.samEngagementRange = 1.00f;
    v.samRadarShutdown = SamRadarShutdown::Always;
    return v;
}

// ---------------------------------------------------------------------------
// Enum parsers — warn on unknown string, return fallback field value
// ---------------------------------------------------------------------------

static FlightAssists parseFlightAssists(const char* s, FlightAssists fb, ILogger& log) {
    if (std::strcmp(s, "all_on") == 0)
        return FlightAssists::AllOn;
    if (std::strcmp(s, "g_limiter_only") == 0)
        return FlightAssists::GLimiterOnly;
    if (std::strcmp(s, "all_off") == 0)
        return FlightAssists::AllOff;
    log.log(LogLevel::Warn, __FILE__, __LINE__,
            (std::string("difficulty: unknown flight_assists '") + s + "', using default").c_str());
    return fb;
}

static EnemyLabels parseEnemyLabels(const char* s, EnemyLabels fb, ILogger& log) {
    if (std::strcmp(s, "always") == 0)
        return EnemyLabels::Always;
    if (std::strcmp(s, "on_lock") == 0)
        return EnemyLabels::OnLock;
    if (std::strcmp(s, "off") == 0)
        return EnemyLabels::Off;
    log.log(LogLevel::Warn, __FILE__, __LINE__,
            (std::string("difficulty: unknown enemy_labels '") + s + "', using default").c_str());
    return fb;
}

static RadarRealism parseRadarRealism(const char* s, RadarRealism fb, ILogger& log) {
    if (std::strcmp(s, "simple") == 0)
        return RadarRealism::Simple;
    if (std::strcmp(s, "standard") == 0)
        return RadarRealism::Standard;
    if (std::strcmp(s, "full") == 0)
        return RadarRealism::Full;
    log.log(LogLevel::Warn, __FILE__, __LINE__,
            (std::string("difficulty: unknown radar_realism '") + s + "', using default").c_str());
    return fb;
}

static RefuelingMode parseRefuelingMode(const char* s, RefuelingMode fb, ILogger& log) {
    if (std::strcmp(s, "auto") == 0)
        return RefuelingMode::Auto;
    if (std::strcmp(s, "simplified") == 0)
        return RefuelingMode::Simplified;
    if (std::strcmp(s, "manual") == 0)
        return RefuelingMode::Manual;
    log.log(LogLevel::Warn, __FILE__, __LINE__,
            (std::string("difficulty: unknown in_flight_refueling '") + s + "', using default").c_str());
    return fb;
}

static RearmMode parseRearmMode(const char* s, RearmMode fb, ILogger& log) {
    if (std::strcmp(s, "instantaneous") == 0)
        return RearmMode::Instantaneous;
    if (std::strcmp(s, "timed") == 0)
        return RearmMode::Timed;
    if (std::strcmp(s, "supply_limited") == 0)
        return RearmMode::SupplyLimited;
    log.log(LogLevel::Warn, __FILE__, __LINE__,
            (std::string("difficulty: unknown rearm_mode '") + s + "', using default").c_str());
    return fb;
}

static CountermeasureUse parseCountermeasureUse(const char* s, CountermeasureUse fb, ILogger& log) {
    if (std::strcmp(s, "never") == 0)
        return CountermeasureUse::Never;
    if (std::strcmp(s, "reactive") == 0)
        return CountermeasureUse::Reactive;
    if (std::strcmp(s, "proactive") == 0)
        return CountermeasureUse::Proactive;
    log.log(LogLevel::Warn, __FILE__, __LINE__,
            (std::string("difficulty: unknown countermeasure_use '") + s + "', using default").c_str());
    return fb;
}

static EnergyManagement parseEnergyManagement(const char* s, EnergyManagement fb, ILogger& log) {
    if (std::strcmp(s, "passive") == 0)
        return EnergyManagement::Passive;
    if (std::strcmp(s, "standard") == 0)
        return EnergyManagement::Standard;
    if (std::strcmp(s, "aggressive_bfm") == 0)
        return EnergyManagement::AggressiveBfm;
    log.log(LogLevel::Warn, __FILE__, __LINE__,
            (std::string("difficulty: unknown energy_management '") + s + "', using default").c_str());
    return fb;
}

static SamRadarShutdown parseSamRadarShutdown(const char* s, SamRadarShutdown fb, ILogger& log) {
    if (std::strcmp(s, "never") == 0)
        return SamRadarShutdown::Never;
    if (std::strcmp(s, "sometimes") == 0)
        return SamRadarShutdown::Sometimes;
    if (std::strcmp(s, "always") == 0)
        return SamRadarShutdown::Always;
    log.log(LogLevel::Warn, __FILE__, __LINE__,
            (std::string("difficulty: unknown sam_radar_shutdown '") + s + "', using default").c_str());
    return fb;
}

// ---------------------------------------------------------------------------
// Float clamper
// ---------------------------------------------------------------------------

static float clampedFloat(toml::node_view<const toml::node> node, float lo, float hi, const char* field, float fallback,
                          ILogger& log) {
    auto v = node.value<double>();
    if (!v)
        return fallback;
    if (*v < static_cast<double>(lo) || *v > static_cast<double>(hi))
        log.log(LogLevel::Warn, __FILE__, __LINE__,
                (std::string("difficulty: ") + field + " out of range, clamping").c_str());
    return static_cast<float>(std::clamp(*v, static_cast<double>(lo), static_cast<double>(hi)));
}

// ---------------------------------------------------------------------------
// Section parser
// ---------------------------------------------------------------------------

static PresetValues parseSection(const toml::table& root, const char* key, const PresetValues& fb, ILogger& log) {
    auto sec = root[key];
    if (!sec || !sec.as_table()) {
        log.log(LogLevel::Warn, __FILE__, __LINE__,
                (std::string("difficulty: missing [") + key + "] section, using defaults").c_str());
        return fb;
    }

    PresetValues v = fb;

    if (auto s = sec["flight_assists"].value<std::string>())
        v.flightAssists = parseFlightAssists(s->c_str(), fb.flightAssists, log);
    v.aimAssist = sec["aim_assist"].value_or(fb.aimAssist);
    if (auto s = sec["enemy_labels"].value<std::string>())
        v.enemyLabels = parseEnemyLabels(s->c_str(), fb.enemyLabels, log);
    if (auto s = sec["radar_realism"].value<std::string>())
        v.radarRealism = parseRadarRealism(s->c_str(), fb.radarRealism, log);
    v.blackoutRedout = sec["blackout_redout"].value_or(fb.blackoutRedout);
    v.fuelConsumption = sec["fuel_consumption"].value_or(fb.fuelConsumption);
    if (auto s = sec["in_flight_refueling"].value<std::string>())
        v.inFlightRefueling = parseRefuelingMode(s->c_str(), fb.inFlightRefueling, log);
    v.friendlyFire = sec["friendly_fire"].value_or(fb.friendlyFire);
    v.crashDamage = sec["crash_damage"].value_or(fb.crashDamage);
    if (auto s = sec["rearm_mode"].value<std::string>())
        v.rearmMode = parseRearmMode(s->c_str(), fb.rearmMode, log);

    v.reactionTimeS = clampedFloat(sec["reaction_time_s"], 0.f, 10.f, "reaction_time_s", fb.reactionTimeS, log);
    v.aimErrorDeg = clampedFloat(sec["aim_error_deg"], 0.f, 90.f, "aim_error_deg", fb.aimErrorDeg, log);
    v.radarSensorRange =
        clampedFloat(sec["radar_sensor_range"], 0.f, 1.f, "radar_sensor_range", fb.radarSensorRange, log);
    if (auto s = sec["countermeasure_use"].value<std::string>())
        v.countermeasureUse = parseCountermeasureUse(s->c_str(), fb.countermeasureUse, log);
    if (auto s = sec["energy_management"].value<std::string>())
        v.energyManagement = parseEnergyManagement(s->c_str(), fb.energyManagement, log);
    v.samEngagementRange =
        clampedFloat(sec["sam_engagement_range"], 0.f, 1.f, "sam_engagement_range", fb.samEngagementRange, log);
    if (auto s = sec["sam_radar_shutdown"].value<std::string>())
        v.samRadarShutdown = parseSamRadarShutdown(s->c_str(), fb.samRadarShutdown, log);

    return v;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

DifficultyMultipliers DifficultyMultipliers::defaults() {
    DifficultyMultipliers dm;
    dm.m_cadet = hardcodedCadet();
    dm.m_pilot = hardcodedPilot();
    dm.m_ace = hardcodedAce();
    return dm;
}

DifficultyMultipliers DifficultyMultipliers::parseFrom(std::string_view text, ILogger& logger) {
    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (const toml::parse_error& e) {
        logger.log(LogLevel::Warn, __FILE__, __LINE__,
                   (std::string("difficulty: failed to parse difficulty.toml: ") + e.what()).c_str());
        return defaults();
    }

    DifficultyMultipliers dm;
    dm.m_cadet = parseSection(tbl, "cadet", hardcodedCadet(), logger);
    dm.m_pilot = parseSection(tbl, "pilot", hardcodedPilot(), logger);
    dm.m_ace = parseSection(tbl, "ace", hardcodedAce(), logger);
    return dm;
}

DifficultyMultipliers DifficultyMultipliers::load(IFilesystem& fs, ILogger& logger) {
    int handle = fs.openFile(PathDomain::Assets, "data/difficulty.toml", false);
    if (handle < 0)
        return defaults();

    std::size_t size = fs.getFileSize(handle);
    std::string content(size, '\0');
    fs.readFile(handle, content.data(), size);
    fs.closeFile(handle);

    return parseFrom(content, logger);
}

DifficultyMultipliers DifficultyMultipliers::load(AssetManager& am, IFilesystem& fs, ILogger& logger) {
    if (auto text = am.loadConfig("difficulty.toml"))
        return parseFrom(*text, logger);
    return load(fs, logger);
}

const PresetValues& DifficultyMultipliers::preset(DifficultyPreset p) const {
    switch (p) {
    case DifficultyPreset::Cadet:
        return m_cadet;
    case DifficultyPreset::Pilot:
        return m_pilot;
    case DifficultyPreset::Ace:
        return m_ace;
    case DifficultyPreset::Custom:
        assert(false && "preset() called with DifficultyPreset::Custom");
        return m_cadet;
    }
    return m_cadet; // unreachable; suppresses MSVC C4715
}

void DifficultyMultipliers::applyPreset(DifficultyPreset p, DifficultySettings& ds) const {
    const PresetValues* v = nullptr;
    switch (p) {
    case DifficultyPreset::Cadet:
        v = &m_cadet;
        break;
    case DifficultyPreset::Pilot:
        v = &m_pilot;
        break;
    case DifficultyPreset::Ace:
        v = &m_ace;
        break;
    case DifficultyPreset::Custom:
        assert(false && "applyPreset() called with DifficultyPreset::Custom");
        return;
    }

    const bool savedInvuln = ds.toggles.invulnerability;
    const bool savedUnlimited = ds.toggles.unlimitedWeapons;

    ds.preset = p;
    ds.toggles.flightAssists = v->flightAssists;
    ds.toggles.aimAssist = v->aimAssist;
    ds.toggles.enemyLabels = v->enemyLabels;
    ds.toggles.radarRealism = v->radarRealism;
    ds.toggles.blackoutRedout = v->blackoutRedout;
    ds.toggles.fuelConsumption = v->fuelConsumption;
    ds.toggles.inFlightRefueling = v->inFlightRefueling;
    ds.toggles.friendlyFire = v->friendlyFire;
    ds.toggles.crashDamage = v->crashDamage;
    ds.toggles.rearmMode = v->rearmMode;
    ds.ai.reactionTimeS = v->reactionTimeS;
    ds.ai.aimErrorDeg = v->aimErrorDeg;
    ds.ai.radarSensorRange = v->radarSensorRange;
    ds.ai.countermeasureUse = v->countermeasureUse;
    ds.ai.energyManagement = v->energyManagement;
    ds.ai.samEngagementRange = v->samEngagementRange;
    ds.ai.samRadarShutdown = v->samRadarShutdown;

    ds.toggles.invulnerability = savedInvuln;
    ds.toggles.unlimitedWeapons = savedUnlimited;
}

} // namespace fl
