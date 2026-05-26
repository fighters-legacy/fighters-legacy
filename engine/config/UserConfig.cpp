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

    // Insertion order determines TOML section order: first_run, engine, graphics, audio
    toml::table root;
    root.insert_or_assign("first_run", std::move(firstRun));
    root.insert_or_assign("engine", std::move(engine));
    root.insert_or_assign("graphics", std::move(graphics));
    root.insert_or_assign("audio", std::move(audio));

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
