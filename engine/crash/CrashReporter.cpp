// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <cstdlib> // _set_abort_behavior
#include <windows.h>
#endif

#include "CrashReporter.h"
#include "FileLogger.h"
#include "IWindow.h"
#include "Version.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// CrashInfo::populateOS
// ---------------------------------------------------------------------------

void CrashInfo::populateOS() {
#if defined(_WIN32)
    OSVERSIONINFOEXA osvi{};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    // RtlGetVersion does not lie about the real version (unlike GetVersionEx post-8.1)
    using RtlGetVersion_t = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto rtl = ntdll ? reinterpret_cast<RtlGetVersion_t>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    RTL_OSVERSIONINFOW rv{};
    rv.dwOSVersionInfoSize = sizeof(rv);
    if (rtl && rtl(&rv) == 0) {
        std::snprintf(osInfo, sizeof(osInfo), "Windows %lu.%lu.%lu", rv.dwMajorVersion, rv.dwMinorVersion,
                      rv.dwBuildNumber);
    } else {
        std::snprintf(osInfo, sizeof(osInfo), "Windows (version unknown)");
    }
#elif defined(__APPLE__)
    char ver[64]{};
    std::size_t len = sizeof(ver);
    if (sysctlbyname("kern.osproductversion", ver, &len, nullptr, 0) == 0)
        std::snprintf(osInfo, sizeof(osInfo), "macOS %s", ver);
    else
        std::snprintf(osInfo, sizeof(osInfo), "macOS (version unknown)");
#else
    // Try /etc/os-release first for a friendly distro name
    std::ifstream f("/etc/os-release");
    std::string prettyName;
    if (f) {
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("PRETTY_NAME=", 0) == 0) {
                prettyName = line.substr(12);
                if (!prettyName.empty() && prettyName.front() == '"')
                    prettyName = prettyName.substr(1);
                if (!prettyName.empty() && prettyName.back() == '"')
                    prettyName.pop_back();
                break;
            }
        }
    }
    if (!prettyName.empty()) {
        std::snprintf(osInfo, sizeof(osInfo), "%s", prettyName.c_str());
    } else {
        utsname uts{};
        uname(&uts);
        std::snprintf(osInfo, sizeof(osInfo), "%s %s", uts.sysname, uts.release);
    }
#endif
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string currentTimestampCompact() {
    using clock = std::chrono::system_clock;
    std::time_t t = clock::to_time_t(clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64]; // 64 bytes: actual output is ~19 chars; larger buf silences GCC format-truncation
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d_%02d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

static std::string signalName(int sig) {
#if defined(_WIN32)
    if (sig == 0)
        return "SEH";
#endif
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGABRT:
        return "SIGABRT";
    case SIGFPE:
        return "SIGFPE";
    case SIGILL:
        return "SIGILL";
#if defined(SIGBUS)
    case SIGBUS:
        return "SIGBUS";
#endif
    }
    return "SIG" + std::to_string(sig);
}

static std::string urlEncode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// PID liveness check (G18)
// ---------------------------------------------------------------------------

bool CrashReporter::isProcessRunning(int pid) {
#if defined(_WIN32)
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h)
        return false;
    DWORD rc = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return rc == WAIT_TIMEOUT; // TIMEOUT = still running; OBJECT_0 = exited
#else
    return ::kill(pid, 0) == 0;
#endif
}

// ---------------------------------------------------------------------------
// Sentinel file
// ---------------------------------------------------------------------------

static fs::path sentinelPath(const std::string& userDataDir) {
    return fs::path(userDataDir) / "state" / "engine.lock";
}

void CrashReporter::createSentinelFile() {
    fs::path p = sentinelPath(m_cfg.userDataDir);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
#if defined(_WIN32)
    int pid = static_cast<int>(GetCurrentProcessId());
#else
    int pid = static_cast<int>(getpid());
#endif
    FILE* f = nullptr;
#if defined(_WIN32)
    fopen_s(&f, p.string().c_str(), "w");
#else
    f = std::fopen(p.string().c_str(), "w");
#endif
    if (f) {
        std::fprintf(f, "%d\n", pid);
        std::fclose(f);
    }
}

void CrashReporter::deleteSentinelFile() {
    std::error_code ec;
    fs::remove(sentinelPath(m_cfg.userDataDir), ec);
}

// ---------------------------------------------------------------------------
// findLatestCrashLog
// ---------------------------------------------------------------------------

std::string CrashReporter::findLatestCrashLog(const std::string& logsDir) {
    std::vector<fs::path> crashes;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(logsDir, ec)) {
        auto name = entry.path().filename().string();
        if (name.rfind("crash_", 0) == 0 && entry.path().extension() == ".log")
            crashes.push_back(entry.path());
    }
    if (crashes.empty())
        return {};
    std::sort(crashes.begin(), crashes.end());
    return crashes.back().string();
}

// ---------------------------------------------------------------------------
// buildGitHubUrl
// ---------------------------------------------------------------------------

std::string CrashReporter::buildGitHubUrl(const std::string& base, const CrashInfo& info, const FileLogger* logger,
                                          const std::string& crashLogPath) {
    std::string title =
        std::string("Crash report: FL v") + (info.engineVersion ? info.engineVersion : "?") + " on " + info.osInfo;

    std::ostringstream body;
    body << "**Engine:** v" << (info.engineVersion ? info.engineVersion : "?") << "\n";
    body << "**OS:** " << info.osInfo << "\n";
    body << "**GPU:** " << info.gpuInfo << "\n";
    body << "**Mods:**";
    if (info.modCount == 0) {
        body << " none";
    } else {
        for (int i = 0; i < info.modCount; ++i)
            body << " " << info.mods[i].id << "@" << info.mods[i].version;
    }
    body << "\n\n";

    if (logger) {
        FileLogger::RingEntry entries[15];
        int n = logger->copyLastLines(entries, 15);
        if (n > 0) {
            body << "**Last log lines:**\n```\n";
            for (int i = 0; i < n; ++i)
                body << entries[i].file << ":" << entries[i].line << "  " << entries[i].message << "\n";
            body << "```\n\n";
        }
    }

    body << "Please attach the full crash log from:\n`" << crashLogPath << "`\n";

    std::string bodyStr = body.str();
    if (bodyStr.size() > 1800)
        bodyStr.resize(1800);

    return base + "?title=" + urlEncode(title) + "&body=" + urlEncode(bodyStr);
}

// ---------------------------------------------------------------------------
// formatCrashHeader
// ---------------------------------------------------------------------------

std::string CrashReporter::formatCrashHeader(int sig) const {
    std::ostringstream s;
    s << "=== Fighters Legacy Crash Report ===\n";
    s << "Signal:      " << signalName(sig) << "\n";
    s << "Version:     " << (m_info.engineVersion ? m_info.engineVersion : "?");
    s << " (" << FL_GIT_HASH << ")\n";
    s << "OS:          " << m_info.osInfo << "\n";
    s << "GPU:         " << m_info.gpuInfo << "\n";
    s << "Session log: " << (m_cfg.logger ? m_cfg.logger->currentLogPath() : "(none)") << "\n";
    s << "Mods:";
    if (m_info.modCount == 0) {
        s << " none";
    } else {
        for (int i = 0; i < m_info.modCount; ++i)
            s << " " << m_info.mods[i].id << "@" << m_info.mods[i].version;
    }
    s << "\n====================================\n";
    return s.str();
}

// ---------------------------------------------------------------------------
// writeCrashDump
// ---------------------------------------------------------------------------

void CrashReporter::writeCrashDump(int sig) {
    fs::path logsDir = fs::path(m_cfg.userDataDir) / "logs";
    std::error_code ec;
    fs::create_directories(logsDir, ec);

    // Rotate crash logs to keep at most 5
    {
        std::vector<fs::path> crashes;
        for (auto& entry : fs::directory_iterator(logsDir, ec)) {
            auto name = entry.path().filename().string();
            if (name.rfind("crash_", 0) == 0 && entry.path().extension() == ".log")
                crashes.push_back(entry.path());
        }
        if (!ec && static_cast<int>(crashes.size()) >= 5) {
            std::sort(crashes.begin(), crashes.end());
            int toDelete = static_cast<int>(crashes.size()) - 5 + 1;
            for (int i = 0; i < toDelete; ++i)
                fs::remove(crashes[static_cast<std::size_t>(i)], ec);
        }
    }

    std::string ts = currentTimestampCompact();
    fs::path dumpPath = logsDir / ("crash_" + ts + ".log");

    FILE* f = nullptr;
#if defined(_WIN32)
    fopen_s(&f, dumpPath.string().c_str(), "wb");
#else
    f = std::fopen(dumpPath.string().c_str(), "wb");
#endif
    if (!f)
        return;

    std::string header = formatCrashHeader(sig);
    std::fwrite(header.data(), 1, header.size(), f);

    if (m_cfg.logger) {
        FileLogger::RingEntry entries[200];
        int n = m_cfg.logger->copyLastLines(entries, 200);
        const char* section = "\n--- Last log entries ---\n";
        std::fwrite(section, 1, std::strlen(section), f);
        for (int i = 0; i < n; ++i) {
            char line[800];
            std::snprintf(line, sizeof(line), "%s:%d  %s\n", entries[i].file, entries[i].line, entries[i].message);
            std::fwrite(line, 1, std::strlen(line), f);
        }
        m_cfg.logger->flush();
    }

    std::fflush(f);
    std::fclose(f);
}

// ---------------------------------------------------------------------------
// Signal handlers
// ---------------------------------------------------------------------------

#if defined(_WIN32)
long __stdcall CrashReporter::win32ExceptionFilter(void* /*exPtrs*/) {
    if (s_crashed.test_and_set())
        return 1; // EXCEPTION_EXECUTE_HANDLER
    if (s_instance)
        s_instance->writeCrashDump(0);
    return 1; // EXCEPTION_EXECUTE_HANDLER — terminate after dump
}
#endif

void CrashReporter::signalHandler(int sig) {
    if (s_crashed.test_and_set()) {
        ::signal(sig, SIG_DFL);
        ::raise(sig);
        return;
    }
    if (s_instance)
        s_instance->writeCrashDump(sig);
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

void CrashReporter::installHandlers() {
    // Do not override sanitizer signal handlers.
    // __has_feature is Clang-only; guard with nested #if so GCC never sees the call.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    return;
#endif
#endif

#if defined(_WIN32)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetUnhandledExceptionFilter(reinterpret_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(win32ExceptionFilter));
    ::signal(SIGABRT, signalHandler);
#else
    ::signal(SIGSEGV, signalHandler);
    ::signal(SIGABRT, signalHandler);
    ::signal(SIGFPE, signalHandler);
    ::signal(SIGILL, signalHandler);
#if defined(__APPLE__)
    ::signal(SIGBUS, signalHandler);
#endif
#endif
}

void CrashReporter::restoreHandlers() {
#if defined(_WIN32)
    SetUnhandledExceptionFilter(nullptr);
    ::signal(SIGABRT, SIG_DFL);
#else
    ::signal(SIGSEGV, SIG_DFL);
    ::signal(SIGABRT, SIG_DFL);
    ::signal(SIGFPE, SIG_DFL);
    ::signal(SIGILL, SIG_DFL);
#if defined(__APPLE__)
    ::signal(SIGBUS, SIG_DFL);
#endif
#endif
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool CrashReporter::init(const Config& cfg, const CrashInfo& info) {
    m_cfg = cfg;
    m_info = info;
    s_instance = this;
    createSentinelFile();
    installHandlers();
    m_initialized = true;
    return true;
}

void CrashReporter::shutdown() {
    if (!m_initialized)
        return;
    restoreHandlers();
    deleteSentinelFile();
    s_instance = nullptr;
    m_initialized = false;
}

void CrashReporter::setMods(const CrashInfo::ModEntry* mods, int count) {
    if (!mods || count <= 0) {
        m_info.modCount = 0;
        return;
    }
    m_info.modCount = std::min(count, CrashInfo::kMaxMods);
    for (int i = 0; i < m_info.modCount; ++i)
        m_info.mods[i] = mods[i];
}

void CrashReporter::setGpuInfo(const char* gpu) {
    if (!gpu)
        return;
    std::size_t gpuLen = std::min(std::strlen(gpu), sizeof(m_info.gpuInfo) - 1);
    std::memcpy(m_info.gpuInfo, gpu, gpuLen);
    m_info.gpuInfo[gpuLen] = '\0';
}

// ---------------------------------------------------------------------------
// checkPreviousCrash (static)
// ---------------------------------------------------------------------------

bool CrashReporter::checkPreviousCrash(const std::string& userDataDir, IWindow* window, FileLogger* logger,
                                       const std::string& githubNewIssueBase) {
    fs::path sentinel = sentinelPath(userDataDir);
    std::error_code ec;
    if (!fs::exists(sentinel, ec) || ec)
        return false;

    // Read PID from sentinel
    int pid = 0;
    {
        FILE* f = nullptr;
#if defined(_WIN32)
        fopen_s(&f, sentinel.string().c_str(), "r");
#else
        f = std::fopen(sentinel.string().c_str(), "r");
#endif
        if (f) {
#if defined(_WIN32)
            fscanf_s(f, "%d", &pid);
#else
            std::fscanf(f, "%d", &pid);
#endif
            std::fclose(f);
        }
    }

    // If the original process is still alive this is a concurrent instance, not a crash
    if (pid > 0 && isProcessRunning(pid)) {
        return false;
    }

    // Confirmed prior crash — find crash log
    std::string logsDir = (fs::path(userDataDir) / "logs").string();
    std::string crashLogPath = findLatestCrashLog(logsDir);

    // Delete sentinel regardless of user action
    fs::remove(sentinel, ec);

    if (!window)
        return true; // headless — no dialog

    std::string msg = "Fighters Legacy crashed in the last session.\n\n";
    if (!crashLogPath.empty())
        msg += "Crash log saved to:\n  " + crashLogPath + "\n\n";
    msg += "What would you like to do?";

    IWindow::MessageBoxButton buttons[3]{
        {0, "View Log"},
        {1, "Report"},
        {2, "Dismiss"},
    };
    int numBtns = crashLogPath.empty() ? 2 : 3;
    if (crashLogPath.empty()) {
        buttons[0] = {1, "Report"};
        buttons[1] = {2, "Dismiss"};
    }

    int clicked = window->showMessageBox(IWindow::MessageBoxType::Warning, "Previous Crash Detected", msg.c_str(),
                                         buttons, numBtns);

    if (!crashLogPath.empty() && clicked == 0) {
        // View Log
        std::string uri = "file:///" + fs::path(crashLogPath).generic_string();
        window->openURL(uri.c_str());
    } else if (clicked == 1) {
        // Report — build a placeholder CrashInfo from available data
        CrashInfo info{};
        info.engineVersion = FL_VERSION_STRING;
        std::string url = buildGitHubUrl(githubNewIssueBase, info, logger, crashLogPath);
        window->openURL(url.c_str());
    }

    return true;
}
