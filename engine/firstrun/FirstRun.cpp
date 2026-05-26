// SPDX-License-Identifier: GPL-3.0-or-later
#include "firstrun/FirstRun.h"

#include "ILogger.h"
#include "config/UserConfig.h"

FirstRun::FirstRun(UserConfig& config, ILogger& logger) : m_config(config), m_logger(logger) {}

FirstRunOutcome FirstRun::check(bool hasContentPacks) const {
    if (!hasContentPacks) {
        if (m_config.isFirstRunCompleted())
            m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                         "no content packs found; config shows first-run complete — launching sandbox inspector");
        else
            m_logger.log(LogLevel::Info, __FILE__, __LINE__,
                         "no content packs found on first run — launching sandbox inspector");
        return FirstRunOutcome::LaunchSandboxInspector;
    }
    return m_config.isFirstRunCompleted() ? FirstRunOutcome::Skip : FirstRunOutcome::ShowWelcome;
}

void FirstRun::complete(WelcomePath path) {
    m_config.setFirstRunCompleted(true);
    if (!m_config.save())
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__, "first-run: failed to persist completed flag");

    if (path == WelcomePath::GetStarted)
        m_logger.log(LogLevel::Info, __FILE__, __LINE__, "first-run: player chose GetStarted — would open mod browser");
    else
        m_logger.log(LogLevel::Info, __FILE__, __LINE__,
                     "first-run: player chose ModDeveloper — would open sandbox mode");
}
