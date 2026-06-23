// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <fcntl.h> // _O_RDONLY
#include <io.h>    // _open_osfhandle, _fdopen
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
// environ is POSIX but not declared in <unistd.h> on macOS; explicit declaration
// is required to use it with posix_spawnp.
extern char** environ;
#endif

#include "ILogger.h"
#include "Subprocess.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace fl {

// ---------------------------------------------------------------------------
// Platform-specific Impl
// ---------------------------------------------------------------------------

struct Subprocess::Impl {
#if defined(_WIN32)
    HANDLE hProcess{INVALID_HANDLE_VALUE};
    HANDLE hStdinWrite{INVALID_HANDLE_VALUE};
    FILE* stdoutFile{nullptr};
#else
    pid_t pid{-1};
    int stdinWriteFd{-1};
    FILE* stdoutFile{nullptr};
#endif

    // Stdout reader: background thread + queue + condvar for readStdoutLine().
    std::thread readerThread;
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::queue<std::string> lineQueue;
    std::atomic<bool> readerDone{false};

    ~Impl() {
        // Ensure reader thread is joined before destructing shared state.
        if (readerThread.joinable())
            readerThread.join();
#if defined(_WIN32)
        if (hStdinWrite != INVALID_HANDLE_VALUE)
            CloseHandle(hStdinWrite);
        if (hProcess != INVALID_HANDLE_VALUE)
            CloseHandle(hProcess);
        if (stdoutFile)
            fclose(stdoutFile);
#else
        if (stdinWriteFd >= 0)
            close(stdinWriteFd);
        if (stdoutFile)
            fclose(stdoutFile);
        // Reap zombie if still alive.
        if (pid > 0) {
            int status;
            waitpid(pid, &status, WNOHANG);
        }
#endif
    }

    void startReaderThread() {
        readerThread = std::thread([this] {
            char buf[4096];
            while (stdoutFile && fgets(buf, sizeof(buf), stdoutFile)) {
                std::string line(buf);
                // Strip trailing newline.
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                    line.pop_back();
                {
                    std::lock_guard<std::mutex> lk(queueMutex);
                    lineQueue.push(std::move(line));
                }
                queueCv.notify_one();
            }
            readerDone.store(true, std::memory_order_release);
            queueCv.notify_all();
        });
    }
};

// ---------------------------------------------------------------------------
// Subprocess implementation
// ---------------------------------------------------------------------------

// Explicit bodies (not = default) so these functions have external linkage and are
// never inlined at call sites where Subprocess::Impl is an incomplete type.
Subprocess::~Subprocess() {} // m_impl destroyed here where Impl is complete
Subprocess::Subprocess(Subprocess&& o) noexcept : m_impl(std::move(o.m_impl)) {}
Subprocess& Subprocess::operator=(Subprocess&& o) noexcept {
    m_impl = std::move(o.m_impl);
    return *this;
}

bool Subprocess::valid() const {
    return m_impl != nullptr;
}

bool Subprocess::isRunning() const {
    if (!m_impl)
        return false;
#if defined(_WIN32)
    if (m_impl->hProcess == INVALID_HANDLE_VALUE)
        return false;
    return WaitForSingleObject(m_impl->hProcess, 0) == WAIT_TIMEOUT;
#else
    if (m_impl->pid <= 0)
        return false;
    int status;
    return waitpid(m_impl->pid, &status, WNOHANG) == 0;
#endif
}

void Subprocess::writeStdin(std::string_view line) {
    if (!m_impl)
        return;
    std::string buf(line);
    buf += '\n';
#if defined(_WIN32)
    DWORD written;
    WriteFile(m_impl->hStdinWrite, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr);
#else
    write(m_impl->stdinWriteFd, buf.data(), buf.size());
#endif
}

std::optional<std::string> Subprocess::readStdoutLine(int timeoutMs) {
    if (!m_impl)
        return std::nullopt;
    std::unique_lock<std::mutex> lk(m_impl->queueMutex);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (m_impl->lineQueue.empty()) {
        if (m_impl->readerDone.load(std::memory_order_acquire))
            return std::nullopt;
        if (m_impl->queueCv.wait_until(lk, deadline) == std::cv_status::timeout)
            return std::nullopt;
    }
    std::string line = std::move(m_impl->lineQueue.front());
    m_impl->lineQueue.pop();
    return line;
}

void Subprocess::stop() {
    if (!m_impl)
        return;
    // Graceful: send "quit" to admin console and wait up to 2 seconds.
    writeStdin("quit");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (isRunning() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Forceful kill if still running.
    if (isRunning()) {
#if defined(_WIN32)
        TerminateProcess(m_impl->hProcess, 1);
#else
        kill(m_impl->pid, SIGKILL);
#endif
    }

    // Close stdin pipe so reader thread sees EOF.
#if defined(_WIN32)
    if (m_impl->hStdinWrite != INVALID_HANDLE_VALUE) {
        CloseHandle(m_impl->hStdinWrite);
        m_impl->hStdinWrite = INVALID_HANDLE_VALUE;
    }
#else
    if (m_impl->stdinWriteFd >= 0) {
        close(m_impl->stdinWriteFd);
        m_impl->stdinWriteFd = -1;
    }
#endif
}

// ---------------------------------------------------------------------------
// spawn() — platform-specific process creation
// ---------------------------------------------------------------------------

Subprocess Subprocess::spawn(const std::string& binaryPath, const std::vector<std::string>& args, bool captureStdout,
                             bool captureStdin, ILogger& log) {
    Subprocess sub;
    sub.m_impl = std::make_unique<Impl>();
    Impl& impl = *sub.m_impl;

#if defined(_WIN32)
    // --- Windows: CreateProcess + pipes ---

    // Full binary path with .exe suffix.
    std::string binFull = binaryPath + ".exe";

    // Build quoted command line: "path" arg1 arg2 ...
    std::string cmdLine = "\"" + binFull + "\"";
    for (const auto& a : args) {
        cmdLine += " ";
        cmdLine += a;
    }

    HANDLE hStdinRead = INVALID_HANDLE_VALUE;
    HANDLE hStdoutRead = INVALID_HANDLE_VALUE;
    HANDLE hStdoutWrite = INVALID_HANDLE_VALUE;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (captureStdin) {
        HANDLE r, w;
        if (!CreatePipe(&r, &w, &sa, 0)) {
            log.log(LogLevel::Error, __FILE__, __LINE__, "Subprocess: CreatePipe stdin failed");
            return {};
        }
        // Write end must NOT be inherited by child.
        SetHandleInformation(w, HANDLE_FLAG_INHERIT, 0);
        hStdinRead = r;
        impl.hStdinWrite = w;
    }
    if (captureStdout) {
        HANDLE r, w;
        if (!CreatePipe(&r, &w, &sa, 0)) {
            log.log(LogLevel::Error, __FILE__, __LINE__, "Subprocess: CreatePipe stdout failed");
            if (hStdinRead != INVALID_HANDLE_VALUE)
                CloseHandle(hStdinRead);
            return {};
        }
        SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
        hStdoutRead = r;
        hStdoutWrite = w;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    if (captureStdin || captureStdout) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = captureStdin ? hStdinRead : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = captureStdout ? hStdoutWrite : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, const_cast<char*>(cmdLine.c_str()), nullptr, nullptr,
                             TRUE, // inherit handles
                             0, nullptr, nullptr, &si, &pi);

    // Close child-side pipe ends in parent.
    if (hStdinRead != INVALID_HANDLE_VALUE)
        CloseHandle(hStdinRead);
    if (hStdoutWrite != INVALID_HANDLE_VALUE)
        CloseHandle(hStdoutWrite);
    CloseHandle(pi.hThread);

    if (!ok) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Subprocess: CreateProcess failed for %s (error %lu)", binFull.c_str(),
                      GetLastError());
        log.log(LogLevel::Error, __FILE__, __LINE__, buf);
        return {};
    }

    impl.hProcess = pi.hProcess;

    if (captureStdout && hStdoutRead != INVALID_HANDLE_VALUE) {
        int fd = _open_osfhandle(reinterpret_cast<intptr_t>(hStdoutRead), 0);
        if (fd >= 0)
            impl.stdoutFile = _fdopen(fd, "r");
        if (!impl.stdoutFile) {
            log.log(LogLevel::Warn, __FILE__, __LINE__, "Subprocess: _fdopen stdout failed");
        }
    }

#else
    // --- POSIX: posix_spawn + pipes ---

    std::string binFull = binaryPath; // no extension on POSIX

    int stdinPipe[2] = {-1, -1};
    int stdoutPipe[2] = {-1, -1};

    if (captureStdin && pipe(stdinPipe) != 0) {
        log.log(LogLevel::Error, __FILE__, __LINE__, "Subprocess: pipe(stdin) failed");
        return {};
    }
    if (captureStdout && pipe(stdoutPipe) != 0) {
        log.log(LogLevel::Error, __FILE__, __LINE__, "Subprocess: pipe(stdout) failed");
        if (captureStdin) {
            close(stdinPipe[0]);
            close(stdinPipe[1]);
        }
        return {};
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (captureStdin) {
        posix_spawn_file_actions_adddup2(&actions, stdinPipe[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions, stdinPipe[0]);
        posix_spawn_file_actions_addclose(&actions, stdinPipe[1]);
    }
    if (captureStdout) {
        posix_spawn_file_actions_adddup2(&actions, stdoutPipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, stdoutPipe[0]);
        posix_spawn_file_actions_addclose(&actions, stdoutPipe[1]);
    }

    // Build argv.
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(binFull.c_str()));
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid;
    // posix_spawnp resolves via PATH (like execvp); posix_spawn requires an absolute path.
    // Using posix_spawnp handles both absolute paths (fl-server) and bare names (sh, cmd).
    int rc = posix_spawnp(&pid, binFull.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);

    // Close child-side pipe ends in parent.
    if (captureStdin)
        close(stdinPipe[0]);
    if (captureStdout)
        close(stdoutPipe[1]);

    if (rc != 0) {
        if (captureStdin)
            close(stdinPipe[1]);
        if (captureStdout)
            close(stdoutPipe[0]);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Subprocess: posix_spawn failed for %s (%s)", binFull.c_str(), strerror(rc));
        log.log(LogLevel::Error, __FILE__, __LINE__, buf);
        return {};
    }

    impl.pid = pid;
    if (captureStdin)
        impl.stdinWriteFd = stdinPipe[1];
    if (captureStdout) {
        impl.stdoutFile = fdopen(stdoutPipe[0], "r");
        if (!impl.stdoutFile) {
            log.log(LogLevel::Warn, __FILE__, __LINE__, "Subprocess: fdopen stdout failed");
            close(stdoutPipe[0]);
        }
    }
#endif

    if (captureStdout && impl.stdoutFile)
        impl.startReaderThread();

    return sub;
}

} // namespace fl
