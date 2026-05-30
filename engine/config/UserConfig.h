// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ILogger.h"
#include "config/AccessibilitySettings.h"
#include "config/AudioSettings.h"
#include "config/DifficultySettings.h"
#include "config/GraphicsSettings.h"

class IFilesystem;

// Free function — also used by the fighters-legacy CLI parser.
LogLevel parseLogLevel(const char* s);

class UserConfig {
  public:
    UserConfig(IFilesystem& fs, ILogger& logger);

    // Returns false if file is missing (normal on first run — no error logged)
    // or if TOML is malformed (Warn logged). True on success.
    bool load();

    // Atomic write: tmp path → renameFile. Returns false on any I/O failure.
    bool save();

    bool isFirstRunCompleted() const;
    void setFirstRunCompleted(bool value);

    // Reads [engine].log_level; default Info. Unknown strings fall back to Info.
    LogLevel logLevel() const;
    void setLogLevel(LogLevel level);

    GraphicsSettings graphics() const;
    void setGraphics(const GraphicsSettings& gs);

    AudioSettings audio() const;
    void setAudio(const AudioSettings& as);

    DifficultySettings difficulty() const;
    void setDifficulty(const DifficultySettings& ds);

    AccessibilitySettings accessibility() const;
    void setAccessibility(const AccessibilitySettings& as);

  private:
    static constexpr const char* kPath = "config/user.toml";
    static constexpr const char* kTmpPath = "config/user.toml.tmp";

    IFilesystem& m_fs;
    ILogger& m_logger;
    bool m_firstRunCompleted = false;
    LogLevel m_logLevel{LogLevel::Info};
    GraphicsSettings m_graphics{};
    AudioSettings m_audio{};
    DifficultySettings m_difficulty{};
    AccessibilitySettings m_accessibility{};
};
