// SPDX-License-Identifier: GPL-3.0-or-later
#include "config/UserConfig.h"

#include "IFilesystem.h"
#include "ILogger.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>

LogLevel parseLogLevel(const char* s) {
    if (!s)
        return LogLevel::Info;
    if (std::strcmp(s, "debug") == 0)
        return LogLevel::Debug;
    if (std::strcmp(s, "info") == 0)
        return LogLevel::Info;
    if (std::strcmp(s, "warn") == 0)
        return LogLevel::Warn;
    if (std::strcmp(s, "error") == 0)
        return LogLevel::Error;
    return LogLevel::Info;
}

static const char* logLevelString(LogLevel l) {
    switch (l) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    }
    return "info";
}

// ---------------------------------------------------------------------------
// Graphics enum helpers
// ---------------------------------------------------------------------------

static const char* vsyncModeString(VsyncMode v) {
    switch (v) {
    case VsyncMode::Off:
        return "off";
    case VsyncMode::On:
        return "on";
    case VsyncMode::Adaptive:
        return "adaptive";
    }
    return "on";
}

static VsyncMode parseVsyncMode(const char* s) {
    if (!s)
        return VsyncMode::On;
    if (std::strcmp(s, "off") == 0)
        return VsyncMode::Off;
    if (std::strcmp(s, "on") == 0)
        return VsyncMode::On;
    if (std::strcmp(s, "adaptive") == 0)
        return VsyncMode::Adaptive;
    return VsyncMode::On;
}

static const char* frameRateCapString(FrameRateCap c) {
    switch (c) {
    case FrameRateCap::Off:
        return "off";
    case FrameRateCap::Cap30:
        return "30";
    case FrameRateCap::Cap60:
        return "60";
    case FrameRateCap::Cap120:
        return "120";
    case FrameRateCap::Cap144:
        return "144";
    case FrameRateCap::Cap240:
        return "240";
    }
    return "off";
}

static FrameRateCap parseFrameRateCap(const char* s) {
    if (!s)
        return FrameRateCap::Off;
    if (std::strcmp(s, "off") == 0)
        return FrameRateCap::Off;
    if (std::strcmp(s, "30") == 0)
        return FrameRateCap::Cap30;
    if (std::strcmp(s, "60") == 0)
        return FrameRateCap::Cap60;
    if (std::strcmp(s, "120") == 0)
        return FrameRateCap::Cap120;
    if (std::strcmp(s, "144") == 0)
        return FrameRateCap::Cap144;
    if (std::strcmp(s, "240") == 0)
        return FrameRateCap::Cap240;
    return FrameRateCap::Off;
}

static const char* qualityLevelString(QualityLevel q) {
    switch (q) {
    case QualityLevel::Low:
        return "low";
    case QualityLevel::Medium:
        return "medium";
    case QualityLevel::High:
        return "high";
    case QualityLevel::Ultra:
        return "ultra";
    }
    return "high";
}

static QualityLevel parseQualityLevel(const char* s) {
    if (!s)
        return QualityLevel::High;
    if (std::strcmp(s, "low") == 0)
        return QualityLevel::Low;
    if (std::strcmp(s, "medium") == 0)
        return QualityLevel::Medium;
    if (std::strcmp(s, "high") == 0)
        return QualityLevel::High;
    if (std::strcmp(s, "ultra") == 0)
        return QualityLevel::Ultra;
    return QualityLevel::High;
}

static const char* drawDistanceString(DrawDistance d) {
    switch (d) {
    case DrawDistance::Low:
        return "low";
    case DrawDistance::Medium:
        return "medium";
    case DrawDistance::High:
        return "high";
    case DrawDistance::Ultra:
        return "ultra";
    }
    return "high";
}

static DrawDistance parseDrawDistance(const char* s) {
    if (!s)
        return DrawDistance::High;
    if (std::strcmp(s, "low") == 0)
        return DrawDistance::Low;
    if (std::strcmp(s, "medium") == 0)
        return DrawDistance::Medium;
    if (std::strcmp(s, "high") == 0)
        return DrawDistance::High;
    if (std::strcmp(s, "ultra") == 0)
        return DrawDistance::Ultra;
    return DrawDistance::High;
}

static int uiScaleInt(UiScale u) {
    switch (u) {
    case UiScale::Scale75:
        return 75;
    case UiScale::Scale100:
        return 100;
    case UiScale::Scale125:
        return 125;
    case UiScale::Scale150:
        return 150;
    }
    return 100;
}

static UiScale parseUiScale(int v) {
    switch (v) {
    case 75:
        return UiScale::Scale75;
    case 100:
        return UiScale::Scale100;
    case 125:
        return UiScale::Scale125;
    case 150:
        return UiScale::Scale150;
    default:
        return UiScale::Scale100;
    }
}

// ---------------------------------------------------------------------------
// Difficulty enum helpers
// ---------------------------------------------------------------------------

static const char* difficultyPresetString(DifficultyPreset p) {
    switch (p) {
    case DifficultyPreset::Cadet:
        return "cadet";
    case DifficultyPreset::Pilot:
        return "pilot";
    case DifficultyPreset::Ace:
        return "ace";
    case DifficultyPreset::Custom:
        return "custom";
    }
    return "cadet";
}

static DifficultyPreset parseDifficultyPreset(const char* s, ILogger& log) {
    if (!s)
        return DifficultyPreset::Cadet;
    if (std::strcmp(s, "cadet") == 0)
        return DifficultyPreset::Cadet;
    if (std::strcmp(s, "pilot") == 0)
        return DifficultyPreset::Pilot;
    if (std::strcmp(s, "ace") == 0)
        return DifficultyPreset::Ace;
    if (std::strcmp(s, "custom") == 0)
        return DifficultyPreset::Custom; // valid, no Warn
    log.log(LogLevel::Warn, __FILE__, __LINE__,
            (std::string("user config: unknown difficulty preset '") + s + "', defaulting to cadet").c_str());
    return DifficultyPreset::Cadet;
}

static const char* flightAssistsString(FlightAssists v) {
    switch (v) {
    case FlightAssists::AllOn:
        return "all_on";
    case FlightAssists::GLimiterOnly:
        return "g_limiter_only";
    case FlightAssists::AllOff:
        return "all_off";
    }
    return "all_on";
}

static FlightAssists parseFlightAssists(const char* s, ILogger& log) {
    if (!s)
        return FlightAssists::AllOn;
    if (std::strcmp(s, "all_on") == 0)
        return FlightAssists::AllOn;
    if (std::strcmp(s, "g_limiter_only") == 0)
        return FlightAssists::GLimiterOnly;
    if (std::strcmp(s, "all_off") == 0)
        return FlightAssists::AllOff;
    log.log(LogLevel::Warn, __FILE__, __LINE__,
            (std::string("user config: unknown flight_assists '") + s + "', defaulting to all_on").c_str());
    return FlightAssists::AllOn;
}

static const char* enemyLabelsString(EnemyLabels v) {
    switch (v) {
    case EnemyLabels::Always:
        return "always";
    case EnemyLabels::OnLock:
        return "on_lock";
    case EnemyLabels::Off:
        return "off";
    }
    return "always";
}

static EnemyLabels parseEnemyLabels(const char* s, ILogger& log) {
    if (!s)
        return EnemyLabels::Always;
    if (std::strcmp(s, "always") == 0)
        return EnemyLabels::Always;
    if (std::strcmp(s, "on_lock") == 0)
        return EnemyLabels::OnLock;
    if (std::strcmp(s, "off") == 0)
        return EnemyLabels::Off;
    log.log(LogLevel::Warn, __FILE__, __LINE__,
            (std::string("user config: unknown enemy_labels '") + s + "', defaulting to always").c_str());
    return EnemyLabels::Always;
}

static const char* radarRealismString(RadarRealism v) {
    switch (v) {
    case RadarRealism::Simple:
        return "simple";
    case RadarRealism::Standard:
        return "standard";
    case RadarRealism::Full:
        return "full";
    }
    return "simple";
}

static RadarRealism parseRadarRealism(const char* s, ILogger& log) {
    if (!s)
        return RadarRealism::Simple;
    if (std::strcmp(s, "simple") == 0)
        return RadarRealism::Simple;
    if (std::strcmp(s, "standard") == 0)
        return RadarRealism::Standard;
    if (std::strcmp(s, "full") == 0)
        return RadarRealism::Full;
    log.log(LogLevel::Warn, __FILE__, __LINE__,
            (std::string("user config: unknown radar_realism '") + s + "', defaulting to simple").c_str());
    return RadarRealism::Simple;
}

static const char* refuelingModeString(RefuelingMode v) {
    switch (v) {
    case RefuelingMode::Auto:
        return "auto";
    case RefuelingMode::Simplified:
        return "simplified";
    case RefuelingMode::Manual:
        return "manual";
    }
    return "auto";
}

static RefuelingMode parseRefuelingMode(const char* s, ILogger& log) {
    if (!s)
        return RefuelingMode::Auto;
    if (std::strcmp(s, "auto") == 0)
        return RefuelingMode::Auto;
    if (std::strcmp(s, "simplified") == 0)
        return RefuelingMode::Simplified;
    if (std::strcmp(s, "manual") == 0)
        return RefuelingMode::Manual;
    log.log(LogLevel::Warn, __FILE__, __LINE__,
            (std::string("user config: unknown in_flight_refueling '") + s + "', defaulting to auto").c_str());
    return RefuelingMode::Auto;
}

static const char* rearmModeString(RearmMode v) {
    switch (v) {
    case RearmMode::Instantaneous:
        return "instantaneous";
    case RearmMode::Timed:
        return "timed";
    case RearmMode::SupplyLimited:
        return "supply_limited";
    }
    return "instantaneous";
}

static RearmMode parseRearmMode(const char* s, ILogger& log) {
    if (!s)
        return RearmMode::Instantaneous;
    if (std::strcmp(s, "instantaneous") == 0)
        return RearmMode::Instantaneous;
    if (std::strcmp(s, "timed") == 0)
        return RearmMode::Timed;
    if (std::strcmp(s, "supply_limited") == 0)
        return RearmMode::SupplyLimited;
    log.log(LogLevel::Warn, __FILE__, __LINE__,
            (std::string("user config: unknown rearm_mode '") + s + "', defaulting to instantaneous").c_str());
    return RearmMode::Instantaneous;
}

static const char* countermeasureUseString(CountermeasureUse v) {
    switch (v) {
    case CountermeasureUse::Never:
        return "never";
    case CountermeasureUse::Reactive:
        return "reactive";
    case CountermeasureUse::Proactive:
        return "proactive";
    }
    return "never";
}

static CountermeasureUse parseCountermeasureUse(const char* s, ILogger& log) {
    if (!s)
        return CountermeasureUse::Never;
    if (std::strcmp(s, "never") == 0)
        return CountermeasureUse::Never;
    if (std::strcmp(s, "reactive") == 0)
        return CountermeasureUse::Reactive;
    if (std::strcmp(s, "proactive") == 0)
        return CountermeasureUse::Proactive;
    log.log(LogLevel::Warn, __FILE__, __LINE__,
            (std::string("user config: unknown countermeasure_use '") + s + "', defaulting to never").c_str());
    return CountermeasureUse::Never;
}

static const char* energyManagementString(EnergyManagement v) {
    switch (v) {
    case EnergyManagement::Passive:
        return "passive";
    case EnergyManagement::Standard:
        return "standard";
    case EnergyManagement::AggressiveBfm:
        return "aggressive_bfm";
    }
    return "passive";
}

static EnergyManagement parseEnergyManagement(const char* s, ILogger& log) {
    if (!s)
        return EnergyManagement::Passive;
    if (std::strcmp(s, "passive") == 0)
        return EnergyManagement::Passive;
    if (std::strcmp(s, "standard") == 0)
        return EnergyManagement::Standard;
    if (std::strcmp(s, "aggressive_bfm") == 0)
        return EnergyManagement::AggressiveBfm;
    log.log(LogLevel::Warn, __FILE__, __LINE__,
            (std::string("user config: unknown energy_management '") + s + "', defaulting to passive").c_str());
    return EnergyManagement::Passive;
}

static const char* samRadarShutdownString(SamRadarShutdown v) {
    switch (v) {
    case SamRadarShutdown::Never:
        return "never";
    case SamRadarShutdown::Sometimes:
        return "sometimes";
    case SamRadarShutdown::Always:
        return "always";
    }
    return "never";
}

static SamRadarShutdown parseSamRadarShutdown(const char* s, ILogger& log) {
    if (!s)
        return SamRadarShutdown::Never;
    if (std::strcmp(s, "never") == 0)
        return SamRadarShutdown::Never;
    if (std::strcmp(s, "sometimes") == 0)
        return SamRadarShutdown::Sometimes;
    if (std::strcmp(s, "always") == 0)
        return SamRadarShutdown::Always;
    log.log(LogLevel::Warn, __FILE__, __LINE__,
            (std::string("user config: unknown sam_radar_shutdown '") + s + "', defaulting to never").c_str());
    return SamRadarShutdown::Never;
}

// ---------------------------------------------------------------------------

UserConfig::UserConfig(IFilesystem& fs, ILogger& logger) : m_fs(fs), m_logger(logger) {}

bool UserConfig::load() {
    int handle = m_fs.openFile(PathDomain::UserData, kPath, false);
    if (handle < 0)
        return false;

    std::size_t size = m_fs.getFileSize(handle);
    std::string content(size, '\0');
    m_fs.readFile(handle, content.data(), size);
    m_fs.closeFile(handle);

    toml::table tbl;
    try {
        tbl = toml::parse(content);
    } catch (const toml::parse_error& e) {
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                     (std::string("user config: failed to parse '") + kPath + "': " + e.what()).c_str());
        return false;
    }

    m_firstRunCompleted = tbl["first_run"]["completed"].value_or(false);

    if (auto lvl = tbl["engine"]["log_level"].value<std::string>()) {
        LogLevel parsed = parseLogLevel(lvl->c_str());
        if (parsed == LogLevel::Info && *lvl != "info") {
            m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                         ("user config: unknown log_level '" + *lvl + "', defaulting to info").c_str());
        }
        m_logLevel = parsed;
    }

    // [graphics]
    m_graphics.resolutionWidth = std::max(0, static_cast<int>(tbl["graphics"]["resolution_width"].value_or(0LL)));
    m_graphics.resolutionHeight = std::max(0, static_cast<int>(tbl["graphics"]["resolution_height"].value_or(0LL)));
    if ((m_graphics.resolutionWidth == 0) != (m_graphics.resolutionHeight == 0)) {
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                     "user config: resolution_width and resolution_height must both be 0 (native) "
                     "or both be positive; ignoring and using native resolution");
        m_graphics.resolutionWidth = 0;
        m_graphics.resolutionHeight = 0;
    }

    if (auto v = tbl["graphics"]["vsync"].value<std::string>()) {
        VsyncMode parsed = parseVsyncMode(v->c_str());
        if (parsed == VsyncMode::On && *v != "on")
            m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                         ("user config: unknown vsync '" + *v + "', defaulting to on").c_str());
        m_graphics.vsync = parsed;
    }

    if (auto v = tbl["graphics"]["frame_rate_cap"].value<std::string>()) {
        FrameRateCap parsed = parseFrameRateCap(v->c_str());
        if (parsed == FrameRateCap::Off && *v != "off")
            m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                         ("user config: unknown frame_rate_cap '" + *v + "', defaulting to off").c_str());
        m_graphics.frameRateCap = parsed;
    }

    if (auto v = tbl["graphics"]["quality_preset"].value<std::string>()) {
        QualityLevel parsed = parseQualityLevel(v->c_str());
        if (parsed == QualityLevel::High && *v != "high")
            m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                         ("user config: unknown quality_preset '" + *v + "', defaulting to high").c_str());
        m_graphics.qualityPreset = parsed;
    }

    if (auto v = tbl["graphics"]["draw_distance"].value<std::string>()) {
        DrawDistance parsed = parseDrawDistance(v->c_str());
        if (parsed == DrawDistance::High && *v != "high")
            m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                         ("user config: unknown draw_distance '" + *v + "', defaulting to high").c_str());
        m_graphics.drawDistance = parsed;
    }

    m_graphics.antiAliasing = tbl["graphics"]["anti_aliasing"].value_or(true);

    if (auto v = tbl["graphics"]["ui_scale"].value<int64_t>()) {
        UiScale parsed = parseUiScale(static_cast<int>(*v));
        if (uiScaleInt(parsed) != static_cast<int>(*v))
            m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                         ("user config: unknown ui_scale '" + std::to_string(*v) + "', defaulting to 100").c_str());
        m_graphics.uiScale = parsed;
    }

    if (auto v = tbl["graphics"]["cockpit_fov"].value<int64_t>()) {
        if (*v < 60 || *v > 120)
            m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                         "user config: cockpit_fov out of range [60, 120]; clamping");
        m_graphics.cockpitFov = std::clamp(static_cast<int>(*v), 60, 120);
    }

    // [audio] — TOML stores integers 0-100; struct holds float 0.0-1.0
    auto loadVolume = [&](const char* key, float defaultVal) -> float {
        int defaultInt = static_cast<int>(std::lround(defaultVal * 100.0f));
        int raw = static_cast<int>(tbl["audio"][key].value_or(static_cast<int64_t>(defaultInt)));
        return static_cast<float>(std::clamp(raw, 0, 100)) / 100.0f;
    };
    m_audio.masterVolume = loadVolume("master_volume", 0.80f);
    m_audio.sfxVolume = loadVolume("sfx_volume", 1.00f);
    m_audio.musicVolume = loadVolume("music_volume", 0.70f);
    m_audio.voiceChatVolume = loadVolume("voice_chat_volume", 1.00f);
    m_audio.rwrVolume = loadVolume("rwr_volume", 1.00f);

    // [difficulty] — missing section is normal on first run; defaults are already set
    if (auto v = tbl["difficulty"]["preset"].value<std::string>())
        m_difficulty.preset = parseDifficultyPreset(v->c_str(), m_logger);

    if (auto v = tbl["difficulty"]["flight_assists"].value<std::string>())
        m_difficulty.toggles.flightAssists = parseFlightAssists(v->c_str(), m_logger);
    m_difficulty.toggles.aimAssist = tbl["difficulty"]["aim_assist"].value_or(m_difficulty.toggles.aimAssist);
    m_difficulty.toggles.invulnerability = tbl["difficulty"]["invulnerability"].value_or(false);
    m_difficulty.toggles.unlimitedWeapons = tbl["difficulty"]["unlimited_weapons"].value_or(false);
    if (auto v = tbl["difficulty"]["enemy_labels"].value<std::string>())
        m_difficulty.toggles.enemyLabels = parseEnemyLabels(v->c_str(), m_logger);
    if (auto v = tbl["difficulty"]["radar_realism"].value<std::string>())
        m_difficulty.toggles.radarRealism = parseRadarRealism(v->c_str(), m_logger);
    m_difficulty.toggles.blackoutRedout =
        tbl["difficulty"]["blackout_redout"].value_or(m_difficulty.toggles.blackoutRedout);
    m_difficulty.toggles.fuelConsumption =
        tbl["difficulty"]["fuel_consumption"].value_or(m_difficulty.toggles.fuelConsumption);
    if (auto v = tbl["difficulty"]["in_flight_refueling"].value<std::string>())
        m_difficulty.toggles.inFlightRefueling = parseRefuelingMode(v->c_str(), m_logger);
    m_difficulty.toggles.friendlyFire = tbl["difficulty"]["friendly_fire"].value_or(m_difficulty.toggles.friendlyFire);
    m_difficulty.toggles.crashDamage = tbl["difficulty"]["crash_damage"].value_or(m_difficulty.toggles.crashDamage);
    if (auto v = tbl["difficulty"]["rearm_mode"].value<std::string>())
        m_difficulty.toggles.rearmMode = parseRearmMode(v->c_str(), m_logger);

    if (auto v = tbl["difficulty"]["reaction_time_s"].value<double>())
        m_difficulty.ai.reactionTimeS = static_cast<float>(std::clamp(*v, 0.0, 10.0));
    if (auto v = tbl["difficulty"]["aim_error_deg"].value<double>())
        m_difficulty.ai.aimErrorDeg = static_cast<float>(std::clamp(*v, 0.0, 90.0));
    if (auto v = tbl["difficulty"]["radar_sensor_range"].value<double>())
        m_difficulty.ai.radarSensorRange = static_cast<float>(std::clamp(*v, 0.0, 1.0));
    if (auto v = tbl["difficulty"]["countermeasure_use"].value<std::string>())
        m_difficulty.ai.countermeasureUse = parseCountermeasureUse(v->c_str(), m_logger);
    if (auto v = tbl["difficulty"]["energy_management"].value<std::string>())
        m_difficulty.ai.energyManagement = parseEnergyManagement(v->c_str(), m_logger);
    if (auto v = tbl["difficulty"]["sam_engagement_range"].value<double>())
        m_difficulty.ai.samEngagementRange = static_cast<float>(std::clamp(*v, 0.0, 1.0));
    if (auto v = tbl["difficulty"]["sam_radar_shutdown"].value<std::string>())
        m_difficulty.ai.samRadarShutdown = parseSamRadarShutdown(v->c_str(), m_logger);

    // [accessibility]
    m_accessibility.subtitlesEnabled = tbl["accessibility"]["subtitles"].value_or(true);
    if (auto v = tbl["accessibility"]["subtitle_duration_scale"].value<double>())
        m_accessibility.subtitleDurationScale = static_cast<float>(std::clamp(*v, 0.5, 3.0));

    return true;
}

bool UserConfig::save() {
    if (!m_fs.createDirectory(PathDomain::UserData, "config")) {
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__, "user config: failed to create config directory");
        return false;
    }

    toml::table firstRun;
    firstRun.insert_or_assign("completed", m_firstRunCompleted);

    toml::table engine;
    engine.insert_or_assign("log_level", logLevelString(m_logLevel));

    toml::table graphics;
    graphics.insert_or_assign("resolution_width", static_cast<int64_t>(m_graphics.resolutionWidth));
    graphics.insert_or_assign("resolution_height", static_cast<int64_t>(m_graphics.resolutionHeight));
    graphics.insert_or_assign("vsync", vsyncModeString(m_graphics.vsync));
    graphics.insert_or_assign("frame_rate_cap", frameRateCapString(m_graphics.frameRateCap));
    graphics.insert_or_assign("quality_preset", qualityLevelString(m_graphics.qualityPreset));
    graphics.insert_or_assign("draw_distance", drawDistanceString(m_graphics.drawDistance));
    graphics.insert_or_assign("anti_aliasing", m_graphics.antiAliasing);
    graphics.insert_or_assign("ui_scale", static_cast<int64_t>(uiScaleInt(m_graphics.uiScale)));
    graphics.insert_or_assign("cockpit_fov", static_cast<int64_t>(m_graphics.cockpitFov));

    toml::table audio;
    audio.insert_or_assign("master_volume", static_cast<int64_t>(std::lround(m_audio.masterVolume * 100.0f)));
    audio.insert_or_assign("sfx_volume", static_cast<int64_t>(std::lround(m_audio.sfxVolume * 100.0f)));
    audio.insert_or_assign("music_volume", static_cast<int64_t>(std::lround(m_audio.musicVolume * 100.0f)));
    audio.insert_or_assign("voice_chat_volume", static_cast<int64_t>(std::lround(m_audio.voiceChatVolume * 100.0f)));
    audio.insert_or_assign("rwr_volume", static_cast<int64_t>(std::lround(m_audio.rwrVolume * 100.0f)));

    toml::table difficulty;
    difficulty.insert_or_assign("preset", difficultyPresetString(m_difficulty.preset));
    difficulty.insert_or_assign("flight_assists", flightAssistsString(m_difficulty.toggles.flightAssists));
    difficulty.insert_or_assign("aim_assist", m_difficulty.toggles.aimAssist);
    difficulty.insert_or_assign("invulnerability", m_difficulty.toggles.invulnerability);
    difficulty.insert_or_assign("unlimited_weapons", m_difficulty.toggles.unlimitedWeapons);
    difficulty.insert_or_assign("enemy_labels", enemyLabelsString(m_difficulty.toggles.enemyLabels));
    difficulty.insert_or_assign("radar_realism", radarRealismString(m_difficulty.toggles.radarRealism));
    difficulty.insert_or_assign("blackout_redout", m_difficulty.toggles.blackoutRedout);
    difficulty.insert_or_assign("fuel_consumption", m_difficulty.toggles.fuelConsumption);
    difficulty.insert_or_assign("in_flight_refueling", refuelingModeString(m_difficulty.toggles.inFlightRefueling));
    difficulty.insert_or_assign("friendly_fire", m_difficulty.toggles.friendlyFire);
    difficulty.insert_or_assign("crash_damage", m_difficulty.toggles.crashDamage);
    difficulty.insert_or_assign("rearm_mode", rearmModeString(m_difficulty.toggles.rearmMode));
    difficulty.insert_or_assign("reaction_time_s", static_cast<double>(m_difficulty.ai.reactionTimeS));
    difficulty.insert_or_assign("aim_error_deg", static_cast<double>(m_difficulty.ai.aimErrorDeg));
    difficulty.insert_or_assign("radar_sensor_range", static_cast<double>(m_difficulty.ai.radarSensorRange));
    difficulty.insert_or_assign("countermeasure_use", countermeasureUseString(m_difficulty.ai.countermeasureUse));
    difficulty.insert_or_assign("energy_management", energyManagementString(m_difficulty.ai.energyManagement));
    difficulty.insert_or_assign("sam_engagement_range", static_cast<double>(m_difficulty.ai.samEngagementRange));
    difficulty.insert_or_assign("sam_radar_shutdown", samRadarShutdownString(m_difficulty.ai.samRadarShutdown));

    toml::table accessibility;
    accessibility.insert_or_assign("subtitles", m_accessibility.subtitlesEnabled);
    accessibility.insert_or_assign("subtitle_duration_scale",
                                   static_cast<double>(m_accessibility.subtitleDurationScale));

    // Insertion order determines TOML section order
    toml::table root;
    root.insert_or_assign("first_run", std::move(firstRun));
    root.insert_or_assign("engine", std::move(engine));
    root.insert_or_assign("graphics", std::move(graphics));
    root.insert_or_assign("audio", std::move(audio));
    root.insert_or_assign("difficulty", std::move(difficulty));
    root.insert_or_assign("accessibility", std::move(accessibility));

    std::ostringstream oss;
    oss << root;
    std::string data = oss.str();

    int handle = m_fs.openFile(PathDomain::UserData, kTmpPath, true);
    if (handle < 0) {
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__, "user config: failed to open tmp file for writing");
        return false;
    }
    m_fs.writeFile(handle, data.data(), data.size());
    m_fs.closeFile(handle);

    if (!m_fs.renameFile(PathDomain::UserData, kTmpPath, kPath)) {
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__, "user config: failed to rename tmp file to final path");
        return false;
    }

    return true;
}

bool UserConfig::isFirstRunCompleted() const {
    return m_firstRunCompleted;
}
void UserConfig::setFirstRunCompleted(bool value) {
    m_firstRunCompleted = value;
}

LogLevel UserConfig::logLevel() const {
    return m_logLevel;
}
void UserConfig::setLogLevel(LogLevel level) {
    m_logLevel = level;
}

GraphicsSettings UserConfig::graphics() const {
    return m_graphics;
}
void UserConfig::setGraphics(const GraphicsSettings& gs) {
    m_graphics = gs;
}

AudioSettings UserConfig::audio() const {
    return m_audio;
}
void UserConfig::setAudio(const AudioSettings& as) {
    m_audio = as;
}

DifficultySettings UserConfig::difficulty() const {
    return m_difficulty;
}
void UserConfig::setDifficulty(const DifficultySettings& ds) {
    m_difficulty = ds;
}

AccessibilitySettings UserConfig::accessibility() const {
    return m_accessibility;
}
void UserConfig::setAccessibility(const AccessibilitySettings& as) {
    m_accessibility = as;
}
