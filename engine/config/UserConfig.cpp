// SPDX-License-Identifier: GPL-3.0-or-later
#include "config/UserConfig.h"

#include "IFilesystem.h"
#include "ILogger.h"
#include "util/FsRead.h"

#include "config/TomlNumeric.h"
#include "crypto/RandomToken.h"
#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

namespace fl {

// ---------------------------------------------------------------------------
// Enum <-> string, in one place (#1265)
// ---------------------------------------------------------------------------
//
// Nineteen settings enums each carried a `switch` naming its values and a `strcmp` chain parsing
// them back -- ~550 lines between them, and two halves that had to be edited together forever. A
// value added to one and forgotten in the other silently round-trips to the default, which reaches
// the player as "the game keeps resetting my setting" with nothing in the log.
//
// The names now live in ONE table per enum and both directions read it, so the halves cannot drift.
// tests/test_userconfig_enums.cpp (#1145) drives every value of every enum through save+reload and
// proves that end to end; this makes the property structural rather than only tested for.
//
// A plain table plus two function templates, deliberately NOT an X-macro: no macro precedent exists
// anywhere in engine/, game/ or server/, and the tables stay file-local, so a header would be
// scaffolding for nobody.

template <class E> struct EnumName {
    E value;
    const char* name;
};

// The canonical spelling of a value, and what save() writes. An unlisted value falls back to the
// enum's own default row -- the same behaviour the `switch` forms had after their final `return`.
template <class E, std::size_t N> const char* enumName(E v, const EnumName<E> (&names)[N], E fallback) {
    for (const auto& n : names)
        if (n.value == v)
            return n.name;
    for (const auto& n : names)
        if (n.value == fallback)
            return n.name;
    return names[0].name;
}

// Parse one value, warning when the string is not in the vocabulary. The FIRST row naming a value is
// canonical, so later rows can be legacy input aliases that parse but are never written back.
//
// `log` may be null for the silent path (parseLogLevel is public API and predates the warning). The
// message names the setting and the default it fell back to, because a player who cannot see WHICH
// key was ignored just sees a setting that does not work.
template <class E, std::size_t N>
E parseEnum(const char* s, const EnumName<E> (&names)[N], E fallback, const char* label, ILogger* log) {
    if (!s)
        return fallback;
    for (const auto& n : names)
        if (std::strcmp(s, n.name) == 0)
            return n.value;
    if (log)
        log->log(LogLevel::Warn, __FILE__, __LINE__,
                 (std::string("user config: unknown ") + label + " '" + s + "', defaulting to " +
                  enumName(fallback, names, fallback))
                     .c_str());
    return fallback;
}

constexpr EnumName<LogLevel> kLogLevelNames[] = {
    {LogLevel::Trace, "trace"}, {LogLevel::Debug, "debug"}, {LogLevel::Info, "info"},
    {LogLevel::Warn, "warn"},   {LogLevel::Error, "error"},
};
constexpr LogLevel kLogLevelFallback = LogLevel::Info;

static const char* logLevelString(LogLevel v) {
    return enumName(v, kLogLevelNames, kLogLevelFallback);
}

LogLevel parseLogLevel(const char* s) {
    // Public API (Game.cpp's --log-level): silent, so an unknown value on the command line
    // is not reported as a user-config problem. UserConfig::load passes its own logger.
    return parseEnum(s, kLogLevelNames, kLogLevelFallback, "log_level", nullptr);
}

// ---------------------------------------------------------------------------
// Graphics enum helpers
// ---------------------------------------------------------------------------

constexpr EnumName<VsyncMode> kVsyncNames[] = {
    {VsyncMode::Off, "off"},
    {VsyncMode::On, "on"},
    {VsyncMode::Adaptive, "adaptive"},
};
constexpr VsyncMode kVsyncFallback = VsyncMode::On;

static const char* vsyncModeString(VsyncMode v) {
    return enumName(v, kVsyncNames, kVsyncFallback);
}

static VsyncMode parseVsyncMode(const char* s, ILogger* log) {
    return parseEnum(s, kVsyncNames, kVsyncFallback, "vsync", log);
}

constexpr EnumName<FrameRateCap> kFrameRateCapNames[] = {
    {FrameRateCap::Off, "off"},    {FrameRateCap::Cap30, "30"},   {FrameRateCap::Cap60, "60"},
    {FrameRateCap::Cap120, "120"}, {FrameRateCap::Cap144, "144"}, {FrameRateCap::Cap240, "240"},
};
constexpr FrameRateCap kFrameRateCapFallback = FrameRateCap::Off;

static const char* frameRateCapString(FrameRateCap v) {
    return enumName(v, kFrameRateCapNames, kFrameRateCapFallback);
}

static FrameRateCap parseFrameRateCap(const char* s, ILogger* log) {
    return parseEnum(s, kFrameRateCapNames, kFrameRateCapFallback, "frame_rate_cap", log);
}

constexpr EnumName<QualityLevel> kQualityNames[] = {
    {QualityLevel::Low, "low"},
    {QualityLevel::Medium, "medium"},
    {QualityLevel::High, "high"},
    {QualityLevel::Ultra, "ultra"},
};
constexpr QualityLevel kQualityFallback = QualityLevel::High;

static const char* qualityLevelString(QualityLevel v) {
    return enumName(v, kQualityNames, kQualityFallback);
}

static QualityLevel parseQualityLevel(const char* s, ILogger* log) {
    return parseEnum(s, kQualityNames, kQualityFallback, "quality_preset", log);
}

constexpr EnumName<AntiAliasingMode> kAaNames[] = {
    {AntiAliasingMode::Off, "off"},
    {AntiAliasingMode::FXAA, "fxaa"},
    {AntiAliasingMode::TAA, "taa"},
    // Legacy INPUT aliases, after the canonical rows so they are never written back: MSAA was
    // removed in favour of TAA, and warning a player about a setting this build deleted would be
    // blaming them for our change. Parsing them here is what makes that migration silent.
    {AntiAliasingMode::TAA, "msaa2x"},
    {AntiAliasingMode::TAA, "msaa4x"},
    {AntiAliasingMode::TAA, "msaa8x"},
};
constexpr AntiAliasingMode kAaFallback = AntiAliasingMode::TAA;

static const char* aaModeString(AntiAliasingMode v) {
    return enumName(v, kAaNames, kAaFallback);
}

static AntiAliasingMode parseAaMode(const char* s, ILogger* log) {
    return parseEnum(s, kAaNames, kAaFallback, "aa_mode", log);
}

constexpr EnumName<AmbientOcclusion> kAoNames[] = {
    {AmbientOcclusion::Off, "off"},
    {AmbientOcclusion::Low, "low"},
    {AmbientOcclusion::High, "high"},
};
constexpr AmbientOcclusion kAoFallback = AmbientOcclusion::High;

static const char* aoModeString(AmbientOcclusion v) {
    return enumName(v, kAoNames, kAoFallback);
}

static AmbientOcclusion parseAoMode(const char* s, ILogger* log) {
    return parseEnum(s, kAoNames, kAoFallback, "ao_mode", log);
}

constexpr EnumName<SkyQuality> kSkyNames[] = {
    {SkyQuality::Procedural, "procedural"},
    {SkyQuality::LUT, "lut"},
};
constexpr SkyQuality kSkyFallback = SkyQuality::LUT;

static const char* skyQualityString(SkyQuality v) {
    return enumName(v, kSkyNames, kSkyFallback);
}

static SkyQuality parseSkyQuality(const char* s, ILogger* log) {
    return parseEnum(s, kSkyNames, kSkyFallback, "sky_quality", log);
}

constexpr EnumName<ShadowQuality> kShadowNames[] = {
    {ShadowQuality::Off, "off"},   {ShadowQuality::Low, "low"},     {ShadowQuality::Medium, "medium"},
    {ShadowQuality::High, "high"}, {ShadowQuality::Ultra, "ultra"},
};
constexpr ShadowQuality kShadowFallback = ShadowQuality::High;

static const char* shadowQualityString(ShadowQuality v) {
    return enumName(v, kShadowNames, kShadowFallback);
}

static ShadowQuality parseShadowQuality(const char* s, ILogger* log) {
    return parseEnum(s, kShadowNames, kShadowFallback, "shadow_quality", log);
}

constexpr EnumName<ParticleDensity> kParticleNames[] = {
    {ParticleDensity::Low, "low"},
    {ParticleDensity::Medium, "medium"},
    {ParticleDensity::High, "high"},
    {ParticleDensity::Ultra, "ultra"},
};
constexpr ParticleDensity kParticleFallback = ParticleDensity::High;

static const char* particleDensityString(ParticleDensity v) {
    return enumName(v, kParticleNames, kParticleFallback);
}

static ParticleDensity parseParticleDensity(const char* s, ILogger* log) {
    return parseEnum(s, kParticleNames, kParticleFallback, "particle_density", log);
}

constexpr EnumName<DrawDistance> kDrawDistanceNames[] = {
    {DrawDistance::Low, "low"},
    {DrawDistance::Medium, "medium"},
    {DrawDistance::High, "high"},
    {DrawDistance::Ultra, "ultra"},
};
constexpr DrawDistance kDrawDistanceFallback = DrawDistance::High;

static const char* drawDistanceString(DrawDistance v) {
    return enumName(v, kDrawDistanceNames, kDrawDistanceFallback);
}

static DrawDistance parseDrawDistance(const char* s, ILogger* log) {
    return parseEnum(s, kDrawDistanceNames, kDrawDistanceFallback, "draw_distance", log);
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

constexpr EnumName<DifficultyPreset> kDifficultyNames[] = {
    {DifficultyPreset::Cadet, "cadet"},
    {DifficultyPreset::Pilot, "pilot"},
    {DifficultyPreset::Ace, "ace"},
    {DifficultyPreset::Custom, "custom"},
};
constexpr DifficultyPreset kDifficultyFallback = DifficultyPreset::Cadet;

static const char* difficultyPresetString(DifficultyPreset v) {
    return enumName(v, kDifficultyNames, kDifficultyFallback);
}

static DifficultyPreset parseDifficultyPreset(const char* s, ILogger* log) {
    return parseEnum(s, kDifficultyNames, kDifficultyFallback, "difficulty preset", log);
}

constexpr EnumName<FlightAssists> kFlightAssistsNames[] = {
    {FlightAssists::AllOn, "all_on"},
    {FlightAssists::GLimiterOnly, "g_limiter_only"},
    {FlightAssists::AllOff, "all_off"},
};
constexpr FlightAssists kFlightAssistsFallback = FlightAssists::AllOn;

static const char* flightAssistsString(FlightAssists v) {
    return enumName(v, kFlightAssistsNames, kFlightAssistsFallback);
}

static FlightAssists parseFlightAssists(const char* s, ILogger* log) {
    return parseEnum(s, kFlightAssistsNames, kFlightAssistsFallback, "flight_assists", log);
}

constexpr EnumName<EnemyLabels> kEnemyLabelsNames[] = {
    {EnemyLabels::Always, "always"},
    {EnemyLabels::OnLock, "on_lock"},
    {EnemyLabels::Off, "off"},
};
constexpr EnemyLabels kEnemyLabelsFallback = EnemyLabels::Always;

static const char* enemyLabelsString(EnemyLabels v) {
    return enumName(v, kEnemyLabelsNames, kEnemyLabelsFallback);
}

static EnemyLabels parseEnemyLabels(const char* s, ILogger* log) {
    return parseEnum(s, kEnemyLabelsNames, kEnemyLabelsFallback, "enemy_labels", log);
}

constexpr EnumName<RadarRealism> kRadarRealismNames[] = {
    {RadarRealism::Simple, "simple"},
    {RadarRealism::Standard, "standard"},
    {RadarRealism::Full, "full"},
};
constexpr RadarRealism kRadarRealismFallback = RadarRealism::Simple;

static const char* radarRealismString(RadarRealism v) {
    return enumName(v, kRadarRealismNames, kRadarRealismFallback);
}

static RadarRealism parseRadarRealism(const char* s, ILogger* log) {
    return parseEnum(s, kRadarRealismNames, kRadarRealismFallback, "radar_realism", log);
}

constexpr EnumName<RefuelingMode> kRefuelingNames[] = {
    {RefuelingMode::Auto, "auto"},
    {RefuelingMode::Simplified, "simplified"},
    {RefuelingMode::Manual, "manual"},
};
constexpr RefuelingMode kRefuelingFallback = RefuelingMode::Auto;

static const char* refuelingModeString(RefuelingMode v) {
    return enumName(v, kRefuelingNames, kRefuelingFallback);
}

static RefuelingMode parseRefuelingMode(const char* s, ILogger* log) {
    return parseEnum(s, kRefuelingNames, kRefuelingFallback, "in_flight_refueling", log);
}

constexpr EnumName<RearmMode> kRearmNames[] = {
    {RearmMode::Instantaneous, "instantaneous"},
    {RearmMode::Timed, "timed"},
    {RearmMode::SupplyLimited, "supply_limited"},
};
constexpr RearmMode kRearmFallback = RearmMode::Instantaneous;

static const char* rearmModeString(RearmMode v) {
    return enumName(v, kRearmNames, kRearmFallback);
}

static RearmMode parseRearmMode(const char* s, ILogger* log) {
    return parseEnum(s, kRearmNames, kRearmFallback, "rearm_mode", log);
}

constexpr EnumName<CountermeasureUse> kCountermeasureNames[] = {
    {CountermeasureUse::Never, "never"},
    {CountermeasureUse::Reactive, "reactive"},
    {CountermeasureUse::Proactive, "proactive"},
};
constexpr CountermeasureUse kCountermeasureFallback = CountermeasureUse::Never;

static const char* countermeasureUseString(CountermeasureUse v) {
    return enumName(v, kCountermeasureNames, kCountermeasureFallback);
}

static CountermeasureUse parseCountermeasureUse(const char* s, ILogger* log) {
    return parseEnum(s, kCountermeasureNames, kCountermeasureFallback, "countermeasure_use", log);
}

constexpr EnumName<EnergyManagement> kEnergyNames[] = {
    {EnergyManagement::Passive, "passive"},
    {EnergyManagement::Standard, "standard"},
    {EnergyManagement::AggressiveBfm, "aggressive_bfm"},
};
constexpr EnergyManagement kEnergyFallback = EnergyManagement::Passive;

static const char* energyManagementString(EnergyManagement v) {
    return enumName(v, kEnergyNames, kEnergyFallback);
}

static EnergyManagement parseEnergyManagement(const char* s, ILogger* log) {
    return parseEnum(s, kEnergyNames, kEnergyFallback, "energy_management", log);
}

constexpr EnumName<SamRadarShutdown> kSamRadarNames[] = {
    {SamRadarShutdown::Never, "never"},
    {SamRadarShutdown::Sometimes, "sometimes"},
    {SamRadarShutdown::Always, "always"},
};
constexpr SamRadarShutdown kSamRadarFallback = SamRadarShutdown::Never;

static const char* samRadarShutdownString(SamRadarShutdown v) {
    return enumName(v, kSamRadarNames, kSamRadarFallback);
}

static SamRadarShutdown parseSamRadarShutdown(const char* s, ILogger* log) {
    return parseEnum(s, kSamRadarNames, kSamRadarFallback, "sam_radar_shutdown", log);
}

// ---------------------------------------------------------------------------
// UUID-v4 generator
// ---------------------------------------------------------------------------

static std::string generateUuidV4() {
    // Full-entropy nibbles from the shared token generator (#1233), then the RFC 4122 version and
    // variant nibbles stamped in. Same 8-4-4-4-12 lowercase format as before.
    std::string h = randomHexToken(32);
    auto hexVal = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    h[12] = '4';                         // version 4
    h[16] = "89ab"[hexVal(h[16]) & 0x3]; // variant 1
    return h.substr(0, 8) + '-' + h.substr(8, 4) + '-' + h.substr(12, 4) + '-' + h.substr(16, 4) + '-' + h.substr(20);
}

// ---------------------------------------------------------------------------

UserConfig::UserConfig(IFilesystem& fs, ILogger& logger) : m_fs(fs), m_logger(logger) {}

bool UserConfig::load() {
    const auto read = readFileToString(m_fs, PathDomain::UserData, kPath);
    if (!read)
        return false;
    const std::string& content = *read;

    toml::table tbl;
    try {
        tbl = toml::parse(content);
    } catch (const toml::parse_error& e) {
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                     (std::string("user config: failed to parse '") + kPath + "': " + e.what()).c_str());
        return false;
    }

    m_firstRunCompleted = tbl["first_run"]["completed"].value_or(false);

    if (auto lvl = tbl["engine"]["log_level"].value<std::string>())
        m_logLevel = parseEnum(lvl->c_str(), kLogLevelNames, kLogLevelFallback, "log_level", &m_logger);

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

    if (auto v = tbl["graphics"]["vsync"].value<std::string>())
        m_graphics.vsync = parseVsyncMode(v->c_str(), &m_logger);

    if (auto v = tbl["graphics"]["frame_rate_cap"].value<std::string>())
        m_graphics.frameRateCap = parseFrameRateCap(v->c_str(), &m_logger);

    if (auto v = tbl["graphics"]["quality_preset"].value<std::string>())
        m_graphics.qualityPreset = parseQualityLevel(v->c_str(), &m_logger);

    if (auto v = tbl["graphics"]["draw_distance"].value<std::string>())
        m_graphics.drawDistance = parseDrawDistance(v->c_str(), &m_logger);

    if (auto v = tbl["graphics"]["aa_mode"].value<std::string>()) {
        m_graphics.aaMode = parseAaMode(v->c_str(), &m_logger);
    } else if (auto b = tbl["graphics"]["anti_aliasing"].value<bool>()) {
        // Migrate old bool: true → FXAA, false → Off
        m_graphics.aaMode = *b ? AntiAliasingMode::FXAA : AntiAliasingMode::Off;
    }

    if (auto v = tbl["graphics"]["shadow_quality"].value<std::string>())
        m_graphics.shadowQuality = parseShadowQuality(v->c_str(), &m_logger);

    if (auto v = tbl["graphics"]["particle_density"].value<std::string>())
        m_graphics.particleDensity = parseParticleDensity(v->c_str(), &m_logger);

    if (auto v = tbl["graphics"]["ao_mode"].value<std::string>())
        m_graphics.ambientOcclusion = parseAoMode(v->c_str(), &m_logger);

    if (auto v = tbl["graphics"]["sky_quality"].value<std::string>())
        m_graphics.skyQuality = parseSkyQuality(v->c_str(), &m_logger);

    if (auto v = tomlInt(tbl["graphics"]["ui_scale"])) {
        UiScale parsed = parseUiScale(static_cast<int>(*v));
        if (uiScaleInt(parsed) != static_cast<int>(*v))
            m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                         ("user config: unknown ui_scale '" + std::to_string(*v) + "', defaulting to 100").c_str());
        m_graphics.uiScale = parsed;
    }

    if (auto v = tomlInt(tbl["graphics"]["cockpit_fov"])) {
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
        m_difficulty.preset = parseDifficultyPreset(v->c_str(), &m_logger);

    if (auto v = tbl["difficulty"]["flight_assists"].value<std::string>())
        m_difficulty.toggles.flightAssists = parseFlightAssists(v->c_str(), &m_logger);
    m_difficulty.toggles.aimAssist = tbl["difficulty"]["aim_assist"].value_or(m_difficulty.toggles.aimAssist);
    m_difficulty.toggles.invulnerability = tbl["difficulty"]["invulnerability"].value_or(false);
    m_difficulty.toggles.unlimitedWeapons = tbl["difficulty"]["unlimited_weapons"].value_or(false);
    if (auto v = tbl["difficulty"]["enemy_labels"].value<std::string>())
        m_difficulty.toggles.enemyLabels = parseEnemyLabels(v->c_str(), &m_logger);
    if (auto v = tbl["difficulty"]["radar_realism"].value<std::string>())
        m_difficulty.toggles.radarRealism = parseRadarRealism(v->c_str(), &m_logger);
    m_difficulty.toggles.blackoutRedout =
        tbl["difficulty"]["blackout_redout"].value_or(m_difficulty.toggles.blackoutRedout);
    m_difficulty.toggles.fuelConsumption =
        tbl["difficulty"]["fuel_consumption"].value_or(m_difficulty.toggles.fuelConsumption);
    if (auto v = tbl["difficulty"]["in_flight_refueling"].value<std::string>())
        m_difficulty.toggles.inFlightRefueling = parseRefuelingMode(v->c_str(), &m_logger);
    m_difficulty.toggles.friendlyFire = tbl["difficulty"]["friendly_fire"].value_or(m_difficulty.toggles.friendlyFire);
    m_difficulty.toggles.crashDamage = tbl["difficulty"]["crash_damage"].value_or(m_difficulty.toggles.crashDamage);
    if (auto v = tbl["difficulty"]["rearm_mode"].value<std::string>())
        m_difficulty.toggles.rearmMode = parseRearmMode(v->c_str(), &m_logger);

    if (auto v = tbl["difficulty"]["reaction_time_s"].value<double>())
        m_difficulty.ai.reactionTimeS = static_cast<float>(std::clamp(*v, 0.0, 10.0));
    if (auto v = tbl["difficulty"]["aim_error_deg"].value<double>())
        m_difficulty.ai.aimErrorDeg = static_cast<float>(std::clamp(*v, 0.0, 90.0));
    if (auto v = tbl["difficulty"]["radar_sensor_range"].value<double>())
        m_difficulty.ai.radarSensorRange = static_cast<float>(std::clamp(*v, 0.0, 1.0));
    if (auto v = tbl["difficulty"]["countermeasure_use"].value<std::string>())
        m_difficulty.ai.countermeasureUse = parseCountermeasureUse(v->c_str(), &m_logger);
    if (auto v = tbl["difficulty"]["energy_management"].value<std::string>())
        m_difficulty.ai.energyManagement = parseEnergyManagement(v->c_str(), &m_logger);
    if (auto v = tbl["difficulty"]["sam_engagement_range"].value<double>())
        m_difficulty.ai.samEngagementRange = static_cast<float>(std::clamp(*v, 0.0, 1.0));
    if (auto v = tbl["difficulty"]["sam_radar_shutdown"].value<std::string>())
        m_difficulty.ai.samRadarShutdown = parseSamRadarShutdown(v->c_str(), &m_logger);

    // [accessibility]
    m_accessibility.subtitlesEnabled = tbl["accessibility"]["subtitles"].value_or(true);
    if (auto v = tbl["accessibility"]["subtitle_duration_scale"].value<double>())
        m_accessibility.subtitleDurationScale = static_cast<float>(std::clamp(*v, 0.5, 3.0));

    // [controls]
    // The four hotas_* axis keys are MIGRATION INPUT ONLY since #1061 — they moved into
    // config/bindings.toml as JoystickAxis bindings plus [[axis_config]] entries. They are still read
    // (so a player who retuned them keeps the tuning through the version 2 -> 3 conversion) and
    // deliberately never written back. `present` gates the migration on the section having actually
    // named one of them: with none set, the shipped defaults already say the same thing.
    {
        auto& l = m_controls.legacyHotas;
        const char* keys[] = {"hotas_aileron_axis", "hotas_elevator_axis", "hotas_throttle_axis",
                              "hotas_rudder_axis",  "hotas_deadzone",      "hotas_invert_pitch",
                              "hotas_invert_roll",  "hotas_invert_rudder", "hotas_invert_throttle"};
        for (const char* k : keys)
            l.present = l.present || tbl["controls"][k].type() != toml::node_type::none;
        l.aileronAxis = std::clamp(static_cast<int>(tbl["controls"]["hotas_aileron_axis"].value_or(0LL)), -1, 127);
        l.elevatorAxis = std::clamp(static_cast<int>(tbl["controls"]["hotas_elevator_axis"].value_or(1LL)), -1, 127);
        l.throttleAxis = std::clamp(static_cast<int>(tbl["controls"]["hotas_throttle_axis"].value_or(2LL)), -1, 127);
        l.rudderAxis = std::clamp(static_cast<int>(tbl["controls"]["hotas_rudder_axis"].value_or(3LL)), -1, 127);
        // Clamped below 1.0 because the rescale divides by (1 - deadzone).
        l.deadzone = std::clamp(tbl["controls"]["hotas_deadzone"].value_or(0.05f), 0.0f, 0.99f);
        l.invertPitch = tbl["controls"]["hotas_invert_pitch"].value_or(false);
        l.invertRoll = tbl["controls"]["hotas_invert_roll"].value_or(false);
        l.invertRudder = tbl["controls"]["hotas_invert_rudder"].value_or(false);
        l.invertThrottle = tbl["controls"]["hotas_invert_throttle"].value_or(false);
    }
    m_controls.ffbEnabled = tbl["controls"]["ffb_enabled"].value_or(true); // #928
    if (auto v = tbl["controls"]["ffb_strength"].value<double>())
        m_controls.ffbStrength = std::clamp(static_cast<float>(*v), 0.f, 1.f);

    // [debug]
    if (auto v = tomlInt(tbl["debug"]["overlay_mode"])) {
        switch (*v) {
        case 1:
            m_debug.overlayMode = OverlayMode::Compact;
            break;
        case 2:
            m_debug.overlayMode = OverlayMode::Full;
            break;
        default:
            m_debug.overlayMode = OverlayMode::Off;
            break;
        }
    }

    // [pilot]
    if (auto v = tbl["pilot"]["callsign"].value<std::string>())
        m_pilot.profile.callsign = std::move(*v);
    if (auto v = tbl["pilot"]["guid"].value<std::string>())
        m_pilot.profile.guid = std::move(*v);
    if (auto v = tomlInt(tbl["pilot"]["kills"]))
        m_pilot.profile.kills = static_cast<int>(std::max(int64_t{0}, *v));
    if (auto v = tomlInt(tbl["pilot"]["losses"]))
        m_pilot.profile.losses = static_cast<int>(std::max(int64_t{0}, *v));
    if (auto v = tomlInt(tbl["pilot"]["flight_time_s"]))
        m_pilot.profile.flightTimeS = std::max(int64_t{0}, *v);

    // [pilot.logbook] (#674) — the career record. Arrays are read positionally; missing entries keep 0.
    {
        PilotLogbook& lb = m_pilot.profile.logbook;
        if (auto* arr = tbl["pilot"]["logbook"]["kills_by_class"].as_array()) {
            int i = 0;
            for (auto& e : *arr) {
                if (i >= PilotLogbook::kKillClassCount)
                    break;
                if (auto n = tomlInt(e))
                    lb.killsByClass[i] = static_cast<uint32_t>(std::max(int64_t{0}, *n));
                ++i;
            }
        }
        constexpr int kWc = static_cast<int>(WeaponLogClass::Count);
        auto readWeaponArr = [&](const char* key, uint32_t WeaponAccuracy::* field) {
            if (auto* arr = tbl["pilot"]["logbook"][key].as_array()) {
                int i = 0;
                for (auto& e : *arr) {
                    if (i >= kWc)
                        break;
                    if (auto n = tomlInt(e))
                        lb.weapons[i].*field = static_cast<uint32_t>(std::max(int64_t{0}, *n));
                    ++i;
                }
            }
        };
        readWeaponArr("weapon_shots", &WeaponAccuracy::shots);
        readWeaponArr("weapon_hits", &WeaponAccuracy::hits);
        readWeaponArr("weapon_kills", &WeaponAccuracy::kills);
        if (auto v = tomlInt(tbl["pilot"]["logbook"]["missions_flown"]))
            lb.missionsFlown = static_cast<uint32_t>(std::max(int64_t{0}, *v));
        if (auto v = tomlInt(tbl["pilot"]["logbook"]["missions_failed"]))
            lb.missionsFailed = static_cast<uint32_t>(std::max(int64_t{0}, *v));
        if (auto v = tomlInt(tbl["pilot"]["logbook"]["ejections"]))
            lb.ejections = static_cast<uint32_t>(std::max(int64_t{0}, *v));
        if (auto v = tbl["pilot"]["logbook"]["best_landing"].value<double>())
            lb.bestLandingScore = static_cast<float>(*v);
        if (auto v = tbl["pilot"]["logbook"]["last_landing"].value<double>())
            lb.lastLandingScore = static_cast<float>(*v);
    }

    // [pilot.campaign]
    if (auto v = tbl["pilot"]["campaign"]["active_campaign"].value<std::string>())
        m_pilot.campaign.activeCampaign = std::move(*v);
    if (auto v = tomlInt(tbl["pilot"]["campaign"]["current_mission"]))
        m_pilot.campaign.currentMission = static_cast<int>(std::max(int64_t{0}, *v));
    if (auto* arr = tbl["pilot"]["campaign"]["completed"].as_array()) {
        for (auto& elem : *arr)
            if (auto s = elem.value<std::string>())
                m_pilot.campaign.completed.push_back(std::move(*s));
    }
    if (auto* standings = tbl["pilot"]["campaign"]["faction_standings"].as_table()) {
        for (auto& [k, val] : *standings)
            if (auto n = tomlInt(val))
                m_pilot.campaign.factionStandings[std::string(k)] = static_cast<int>(*n);
    }

    // [client]
    if (auto v = tomlInt(tbl["client"]["motd_display_s"])) {
        if (*v < 0 || *v > 3600)
            m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                         "user config: motd_display_s out of range [0, 3600]; clamping");
        m_client.motdDisplayS = static_cast<uint32_t>(std::clamp(*v, int64_t{0}, int64_t{3600}));
    }
    if (auto v = tbl["client"]["operator_password"].value<std::string>())
        m_client.operatorPassword = std::move(*v);
    if (auto v = tbl["client"]["language"].value<std::string>(); v && !v->empty())
        m_client.language = std::move(*v);
    if (auto v = tbl["client"]["lobby_urls"].value<std::string>())
        m_client.lobbyUrls = std::move(*v);

    // [hud]
    m_hud.showLatency = tbl["hud"]["show_latency"].value_or(true);

    // [prediction]
    m_prediction.enabled = tbl["prediction"]["enabled"].value_or(true);
    if (auto v = tbl["prediction"]["snap_threshold_m"].value<double>())
        m_prediction.snapThresholdM = std::clamp(static_cast<float>(*v), 0.f, 10000.f);
    if (auto v = tbl["prediction"]["blend_rate"].value<double>())
        m_prediction.blendRate = std::clamp(static_cast<float>(*v), 0.f, 1.f);

    // [headtracking] (#927)
    m_headTracking.enabled = tbl["headtracking"]["enabled"].value_or(false);
    m_headTracking.port = std::clamp(static_cast<int>(tbl["headtracking"]["port"].value_or(4242)), 1, 65535);
    if (auto v = tbl["headtracking"]["yaw_scale"].value<double>())
        m_headTracking.yawScale = std::clamp(static_cast<float>(*v), 0.f, 8.f);
    if (auto v = tbl["headtracking"]["pitch_scale"].value<double>())
        m_headTracking.pitchScale = std::clamp(static_cast<float>(*v), 0.f, 8.f);
    if (auto v = tbl["headtracking"]["roll_scale"].value<double>())
        m_headTracking.rollScale = std::clamp(static_cast<float>(*v), 0.f, 8.f);
    if (auto v = tbl["headtracking"]["positional_scale"].value<double>())
        m_headTracking.positionalScale = std::clamp(static_cast<float>(*v), 0.f, 8.f);
    m_headTracking.invertYaw = tbl["headtracking"]["invert_yaw"].value_or(false);
    m_headTracking.invertPitch = tbl["headtracking"]["invert_pitch"].value_or(false);
    m_headTracking.invertRoll = tbl["headtracking"]["invert_roll"].value_or(false);
    if (auto v = tbl["headtracking"]["smoothing"].value<double>())
        m_headTracking.smoothing = std::clamp(static_cast<float>(*v), 0.f, 0.95f);

    // [voice] (Epic J, #531)
    m_voice.enabled = tbl["voice"]["enabled"].value_or(true);
    m_voice.transmitEnabled = tbl["voice"]["transmit"].value_or(true);
    m_voice.inputDevice = tbl["voice"]["input_device"].value_or(std::string{});
    // #935: path to a whisper.cpp model for voice wingman commands. Empty = the tier is off and the
    // radio menu is the path; no model ships with the game.
    m_voice.sttModelPath = tbl["voice"]["stt_model_path"].value_or(std::string{});
    if (auto v = tbl["voice"]["key_mode"].value<std::string>()) {
        if (*v == "vox")
            m_voice.keyMode = VoiceKeyMode::Voice;
        else if (*v == "open")
            m_voice.keyMode = VoiceKeyMode::Open;
        else
            m_voice.keyMode = VoiceKeyMode::PushToTalk;
    }
    if (auto v = tbl["voice"]["vox_threshold"].value<double>())
        m_voice.voxThreshold = std::clamp(static_cast<float>(*v), 0.f, 1.f);
    if (auto v = tbl["voice"]["mic_gain"].value<double>())
        m_voice.micGain = std::clamp(static_cast<float>(*v), 0.f, 4.f);
    m_voice.bitrate = std::clamp(static_cast<int>(tbl["voice"]["bitrate"].value_or(int64_t{24000})), 6000, 128000);
    m_voice.jitterTargetFrames =
        std::clamp(static_cast<int>(tbl["voice"]["jitter_frames"].value_or(int64_t{3})), 1, 12);
    m_voice.radioEffect = tbl["voice"]["radio_effect"].value_or(true);
    m_voice.subtitles = tbl["voice"]["subtitles"].value_or(true);
    if (auto v = tbl["voice"]["ducking"].value<double>())
        m_voice.duckingAmount = std::clamp(static_cast<float>(*v), 0.f, 1.f);
    if (auto* arr = tbl["voice"]["net_volume"].as_array()) {
        for (std::size_t i = 0; i < m_voice.netVolume.size() && i < arr->size(); ++i) {
            if (auto v = arr->get(i)->value<double>())
                m_voice.netVolume[i] = std::clamp(static_cast<float>(*v), 0.f, 2.f);
        }
    }

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
    graphics.insert_or_assign("aa_mode", aaModeString(m_graphics.aaMode));
    graphics.insert_or_assign("shadow_quality", shadowQualityString(m_graphics.shadowQuality));
    graphics.insert_or_assign("particle_density", particleDensityString(m_graphics.particleDensity));
    graphics.insert_or_assign("ao_mode", aoModeString(m_graphics.ambientOcclusion));
    graphics.insert_or_assign("sky_quality", skyQualityString(m_graphics.skyQuality));
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

    // No hotas_* keys are written (#1061): the axes live in config/bindings.toml now, and writing them
    // back would leave two places claiming to own the same mapping — which is the whole defect this
    // migration removes. An existing file's keys are simply left where they are and ignored.
    toml::table controls;
    controls.insert_or_assign("ffb_enabled", m_controls.ffbEnabled); // #928
    controls.insert_or_assign("ffb_strength", static_cast<double>(m_controls.ffbStrength));

    toml::table client;
    client.insert_or_assign("motd_display_s", static_cast<int64_t>(m_client.motdDisplayS));
    if (!m_client.operatorPassword.empty())
        client.insert_or_assign("operator_password", m_client.operatorPassword);
    client.insert_or_assign("language", m_client.language);
    if (!m_client.lobbyUrls.empty())
        client.insert_or_assign("lobby_urls", m_client.lobbyUrls);

    toml::table hud;
    hud.insert_or_assign("show_latency", m_hud.showLatency);

    toml::table prediction;
    prediction.insert_or_assign("enabled", m_prediction.enabled);
    prediction.insert_or_assign("snap_threshold_m", static_cast<double>(m_prediction.snapThresholdM));
    prediction.insert_or_assign("blend_rate", static_cast<double>(m_prediction.blendRate));

    toml::table headtracking; // #927
    headtracking.insert_or_assign("enabled", m_headTracking.enabled);
    headtracking.insert_or_assign("port", static_cast<int64_t>(m_headTracking.port));
    headtracking.insert_or_assign("yaw_scale", static_cast<double>(m_headTracking.yawScale));
    headtracking.insert_or_assign("pitch_scale", static_cast<double>(m_headTracking.pitchScale));
    headtracking.insert_or_assign("roll_scale", static_cast<double>(m_headTracking.rollScale));
    headtracking.insert_or_assign("positional_scale", static_cast<double>(m_headTracking.positionalScale));
    headtracking.insert_or_assign("invert_yaw", m_headTracking.invertYaw);
    headtracking.insert_or_assign("invert_pitch", m_headTracking.invertPitch);
    headtracking.insert_or_assign("invert_roll", m_headTracking.invertRoll);
    headtracking.insert_or_assign("smoothing", static_cast<double>(m_headTracking.smoothing));

    toml::table voice; // Epic J (#531)
    voice.insert_or_assign("enabled", m_voice.enabled);
    voice.insert_or_assign("transmit", m_voice.transmitEnabled);
    voice.insert_or_assign("input_device", m_voice.inputDevice);
    voice.insert_or_assign("stt_model_path", m_voice.sttModelPath);
    voice.insert_or_assign("key_mode", m_voice.keyMode == VoiceKeyMode::Voice  ? "vox"
                                       : m_voice.keyMode == VoiceKeyMode::Open ? "open"
                                                                               : "ptt");
    voice.insert_or_assign("vox_threshold", static_cast<double>(m_voice.voxThreshold));
    voice.insert_or_assign("mic_gain", static_cast<double>(m_voice.micGain));
    voice.insert_or_assign("bitrate", static_cast<int64_t>(m_voice.bitrate));
    voice.insert_or_assign("jitter_frames", static_cast<int64_t>(m_voice.jitterTargetFrames));
    voice.insert_or_assign("radio_effect", m_voice.radioEffect);
    voice.insert_or_assign("subtitles", m_voice.subtitles);
    voice.insert_or_assign("ducking", static_cast<double>(m_voice.duckingAmount));
    {
        toml::array netVol;
        for (const float v : m_voice.netVolume)
            netVol.push_back(static_cast<double>(v));
        voice.insert_or_assign("net_volume", std::move(netVol));
    }

    toml::table debug;
    debug.insert_or_assign("overlay_mode", static_cast<int64_t>(m_debug.overlayMode));

    if (m_pilot.profile.guid.empty())
        m_pilot.profile.guid = generateUuidV4();

    toml::table pilotCampaign;
    pilotCampaign.insert_or_assign("active_campaign", m_pilot.campaign.activeCampaign);
    pilotCampaign.insert_or_assign("current_mission", static_cast<int64_t>(m_pilot.campaign.currentMission));
    toml::array completed;
    for (const auto& id : m_pilot.campaign.completed)
        completed.push_back(id);
    pilotCampaign.insert_or_assign("completed", std::move(completed));
    toml::table factions;
    for (const auto& [k, v] : m_pilot.campaign.factionStandings)
        factions.insert_or_assign(k, static_cast<int64_t>(v));
    pilotCampaign.insert_or_assign("faction_standings", std::move(factions));

    // [pilot.logbook] (#674)
    const PilotLogbook& lb = m_pilot.profile.logbook;
    toml::table logbook;
    {
        toml::array killsByClass;
        for (uint32_t k : lb.killsByClass)
            killsByClass.push_back(static_cast<int64_t>(k));
        logbook.insert_or_assign("kills_by_class", std::move(killsByClass));
        toml::array shots, hits, wkills;
        for (const WeaponAccuracy& wa : lb.weapons) {
            shots.push_back(static_cast<int64_t>(wa.shots));
            hits.push_back(static_cast<int64_t>(wa.hits));
            wkills.push_back(static_cast<int64_t>(wa.kills));
        }
        logbook.insert_or_assign("weapon_shots", std::move(shots));
        logbook.insert_or_assign("weapon_hits", std::move(hits));
        logbook.insert_or_assign("weapon_kills", std::move(wkills));
        logbook.insert_or_assign("missions_flown", static_cast<int64_t>(lb.missionsFlown));
        logbook.insert_or_assign("missions_failed", static_cast<int64_t>(lb.missionsFailed));
        logbook.insert_or_assign("ejections", static_cast<int64_t>(lb.ejections));
        logbook.insert_or_assign("best_landing", static_cast<double>(lb.bestLandingScore));
        logbook.insert_or_assign("last_landing", static_cast<double>(lb.lastLandingScore));
    }

    toml::table pilot;
    pilot.insert_or_assign("callsign", m_pilot.profile.callsign);
    pilot.insert_or_assign("guid", m_pilot.profile.guid);
    pilot.insert_or_assign("kills", static_cast<int64_t>(m_pilot.profile.kills));
    pilot.insert_or_assign("losses", static_cast<int64_t>(m_pilot.profile.losses));
    pilot.insert_or_assign("flight_time_s", m_pilot.profile.flightTimeS);
    pilot.insert_or_assign("logbook", std::move(logbook));
    pilot.insert_or_assign("campaign", std::move(pilotCampaign));

    // Insertion order determines TOML section order
    toml::table root;
    root.insert_or_assign("first_run", std::move(firstRun));
    root.insert_or_assign("engine", std::move(engine));
    root.insert_or_assign("graphics", std::move(graphics));
    root.insert_or_assign("audio", std::move(audio));
    root.insert_or_assign("difficulty", std::move(difficulty));
    root.insert_or_assign("accessibility", std::move(accessibility));
    root.insert_or_assign("controls", std::move(controls));
    root.insert_or_assign("client", std::move(client));
    root.insert_or_assign("hud", std::move(hud));
    root.insert_or_assign("prediction", std::move(prediction));
    root.insert_or_assign("headtracking", std::move(headtracking));
    root.insert_or_assign("voice", std::move(voice));
    root.insert_or_assign("debug", std::move(debug));
    root.insert_or_assign("pilot", std::move(pilot));

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

ClientSettings UserConfig::client() const {
    return m_client;
}
void UserConfig::setClient(const ClientSettings& cs) {
    m_client = cs;
}

HudSettings UserConfig::hud() const {
    return m_hud;
}
void UserConfig::setHud(const HudSettings& hs) {
    m_hud = hs;
}

ControlsSettings UserConfig::controls() const {
    return m_controls;
}
void UserConfig::setControls(const ControlsSettings& cs) {
    m_controls = cs;
}

DebugSettings UserConfig::debug() const {
    return m_debug;
}
void UserConfig::setDebug(const DebugSettings& ds) {
    m_debug = ds;
}

PilotSettings UserConfig::pilot() const {
    return m_pilot;
}
void UserConfig::setPilot(const PilotSettings& ps) {
    m_pilot = ps;
}

PredictionSettings UserConfig::prediction() const {
    return m_prediction;
}
void UserConfig::setPrediction(const PredictionSettings& ps) {
    m_prediction = ps;
}

HeadTrackingSettings UserConfig::headTracking() const {
    return m_headTracking;
}
VoiceSettings UserConfig::voice() const {
    return m_voice;
}
void UserConfig::setVoice(const VoiceSettings& vs) {
    m_voice = vs;
}

void UserConfig::setHeadTracking(const HeadTrackingSettings& hts) {
    m_headTracking = hts;
}

} // namespace fl
