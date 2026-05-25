// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "CrashInfo.h"
#include <atomic>
#include <string>

class FileLogger;
class IWindow;

class CrashReporter {
  public:
    struct Config {
        std::string userDataDir;
        std::string githubNewIssueBase;
        FileLogger* logger{};
        IWindow* window{};
    };

    // Creates sentinel file, installs signal/SEH handlers.
    // Must be called after logger and window are ready.
    bool init(const Config& cfg, const CrashInfo& info);

    // Deletes sentinel file, restores default signal handlers.
    void shutdown();

    // Call at startup before init() — checks for an orphaned sentinel file.
    // If found, shows an OS dialog and returns true. window may be nullptr
    // (headless mode); the dialog is skipped but the sentinel is still cleared.
    static bool checkPreviousCrash(const std::string& userDataDir, IWindow* window, FileLogger* logger,
                                   const std::string& githubNewIssueBase);

    // Update mod list after mods are loaded (for crash dump accuracy).
    void setMods(const CrashInfo::ModEntry* mods, int count);

    // Update GPU info after VkRenderer::init().
    void setGpuInfo(const char* gpu);

    // Extracted for unit-test access; called internally by writeCrashDump().
    std::string formatCrashHeader(int sig) const;

  private:
    inline static CrashReporter* s_instance{nullptr};
    inline static std::atomic_flag s_crashed{};

    static void signalHandler(int sig);
    static bool isProcessRunning(int pid);

    void installHandlers();
    void restoreHandlers();
    void writeCrashDump(int sig);
    void createSentinelFile();
    void deleteSentinelFile();
    static std::string findLatestCrashLog(const std::string& logsDir);
    static std::string buildGitHubUrl(const std::string& base, const CrashInfo& info, const FileLogger* logger,
                                      const std::string& crashLogPath);

#if defined(_WIN32)
    static long __stdcall win32ExceptionFilter(void* exPtrs);
#endif

    Config m_cfg;
    CrashInfo m_info;
    bool m_initialized{false};
};
