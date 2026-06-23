// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fl {

class ILogger;
class UserConfig;

enum class FirstRunOutcome { ShowWelcome, LaunchSandboxInspector, Skip };
enum class WelcomePath { GetStarted, ModDeveloper };

class FirstRun {
  public:
    FirstRun(UserConfig& config, ILogger& logger);

    // Returns LaunchSandboxInspector when hasContentPacks is false (no crash, no block).
    // Otherwise ShowWelcome on first run, Skip when already completed.
    // hasContentPacks defaults to true so existing call sites are unaffected.
    FirstRunOutcome check(bool hasContentPacks = true) const;

    // Sets flag, saves config, logs stub destination.
    void complete(WelcomePath path);

  private:
    UserConfig& m_config;
    ILogger& m_logger;
};

} // namespace fl
