// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class IFilesystem;
class ILogger;

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

  private:
    static constexpr const char* kPath = "config/user.toml";
    static constexpr const char* kTmpPath = "config/user.toml.tmp";

    IFilesystem& m_fs;
    ILogger& m_logger;
    bool m_firstRunCompleted = false;
};
