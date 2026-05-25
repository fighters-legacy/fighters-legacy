// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class ILogger;
class UserConfig;

enum class FirstRunOutcome { ShowWelcome, Skip };
enum class WelcomePath { GetStarted, ModDeveloper };

class FirstRun {
  public:
    FirstRun(UserConfig& config, ILogger& logger);

    // ShowWelcome if first-run flag is not set; Skip otherwise.
    FirstRunOutcome check() const;

    // Sets flag, saves config, logs stub destination.
    void complete(WelcomePath path);

  private:
    UserConfig& m_config;
    ILogger& m_logger;
};
