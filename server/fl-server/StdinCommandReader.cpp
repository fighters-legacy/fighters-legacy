// SPDX-License-Identifier: GPL-3.0-or-later
#include "StdinCommandReader.h"

#include <chrono>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

namespace fl {

namespace {
constexpr std::size_t kReadChunkBytes = 4096;
} // namespace

// ---------------------------------------------------------------------------
// LineAssembler
// ---------------------------------------------------------------------------

void LineAssembler::feed(const char* data, std::size_t len, std::vector<std::string>& out) {
    for (std::size_t i = 0; i < len; ++i) {
        const char c = data[i];
        if (c == '\n') {
            if (!m_overlong) {
                if (!m_partial.empty() && m_partial.back() == '\r')
                    m_partial.pop_back();
                out.push_back(std::move(m_partial));
            }
            m_partial.clear();
            m_overlong = false;
            continue;
        }
        if (m_overlong)
            continue; // swallow the rest of a line that already blew the cap
        if (m_partial.size() >= kMaxLineBytes) {
            // Emit the capped prefix so an operator sees something happened, then discard the tail.
            out.push_back(std::move(m_partial));
            m_partial.clear();
            m_overlong = true;
            continue;
        }
        m_partial.push_back(c);
    }
}

void LineAssembler::flush(std::vector<std::string>& out) {
    if (m_overlong) {
        m_partial.clear();
        m_overlong = false;
        return;
    }
    if (m_partial.empty())
        return;
    if (m_partial.back() == '\r')
        m_partial.pop_back();
    out.push_back(std::move(m_partial));
    m_partial.clear();
}

// ---------------------------------------------------------------------------
// StdinCommandReader
// ---------------------------------------------------------------------------

StdinCommandReader::~StdinCommandReader() {
    stop();
}

void StdinCommandReader::start() {
    if (m_thread.joinable())
        return; // already running (or finished but not yet stopped) -- never overwrite a live thread
    m_state = std::make_shared<State>();
    m_thread = std::thread(&StdinCommandReader::runReadLoop, m_state);
}

void StdinCommandReader::drain(std::vector<std::string>& out) {
    if (!m_state)
        return;
    std::vector<std::string> taken;
    {
        std::lock_guard<std::mutex> lk(m_state->mutex);
        taken.swap(m_state->lines);
    }
    for (auto& line : taken)
        out.push_back(std::move(line));
}

bool StdinCommandReader::eof() const noexcept {
    return m_state && m_state->atEof.load(std::memory_order_acquire);
}

void StdinCommandReader::stop() {
    if (!m_state)
        return;
    m_state->running.store(false, std::memory_order_release);

    if (!m_thread.joinable()) {
        m_state.reset();
        return;
    }

    bool finished = false;
    {
        std::unique_lock<std::mutex> lk(m_state->finishMutex);
        finished = m_state->finishCv.wait_for(lk, std::chrono::milliseconds(kJoinTimeoutMs),
                                              [this]() { return m_state->finished.load(std::memory_order_acquire); });
    }

    if (finished) {
        m_thread.join();
    } else {
        // The only way here is a Windows console read already inside ReadFile on a half-typed line
        // (see the header). Detaching is memory-safe -- the worker owns a shared_ptr to State and
        // nothing else -- and exit-safe, because the worker holds no C-stdio lock for exit-time
        // flushing to block on. That distinction IS the #1038 fix.
        m_thread.detach();
    }
    m_state.reset();
}

// ---------------------------------------------------------------------------
// The platform read loop
// ---------------------------------------------------------------------------

#if defined(_WIN32)

namespace {

// True when a completed line is already sitting in the console input queue, so the ReadFile below
// returns without blocking. Peeking never consumes: cooked-mode ReadFile discards the non-key
// records itself.
bool consoleLinePending(HANDLE h) {
    DWORD queued = 0;
    if (!GetNumberOfConsoleInputEvents(h, &queued) || queued == 0)
        return false;

    constexpr DWORD kPeekCap = 512;
    INPUT_RECORD records[kPeekCap];
    DWORD peeked = 0;
    if (!PeekConsoleInputW(h, records, kPeekCap, &peeked))
        return false;

    for (DWORD i = 0; i < peeked; ++i) {
        const INPUT_RECORD& r = records[i];
        if (r.EventType != KEY_EVENT || !r.Event.KeyEvent.bKeyDown)
            continue;
        const wchar_t ch = r.Event.KeyEvent.uChar.UnicodeChar;
        if (ch == L'\r' || ch == L'\n')
            return true;
    }

    // A line longer than the peek window hides its own terminator. Read it: the operator is typing
    // a 500-character command, and stop() covers the shutdown case by detaching.
    return queued > kPeekCap;
}

} // namespace

void StdinCommandReader::runReadLoop(std::shared_ptr<StdinCommandReader::State> state) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    const DWORD kind = (h == INVALID_HANDLE_VALUE || h == nullptr) ? FILE_TYPE_UNKNOWN : GetFileType(h);
    LineAssembler assembler;
    char buf[kReadChunkBytes];

    if (h == INVALID_HANDLE_VALUE || h == nullptr)
        state->atEof.store(true, std::memory_order_release);

    while (state->running.load(std::memory_order_acquire) && !state->atEof.load(std::memory_order_acquire)) {
        DWORD toRead = static_cast<DWORD>(sizeof(buf));

        if (kind == FILE_TYPE_CHAR) {
            if (WaitForSingleObject(h, static_cast<DWORD>(kPollTimeoutMs)) != WAIT_OBJECT_0)
                continue;
            if (!consoleLinePending(h))
                continue;
        } else if (kind == FILE_TYPE_PIPE) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
                state->atEof.store(true, std::memory_order_release); // broken pipe = writer closed
                break;
            }
            if (avail == 0) {
                Sleep(static_cast<DWORD>(kPollTimeoutMs));
                continue;
            }
            toRead = avail < toRead ? avail : toRead;
        }
        // FILE_TYPE_DISK / anything else: ReadFile returns promptly (data or EOF).

        DWORD got = 0;
        if (!ReadFile(h, buf, toRead, &got, nullptr)) {
            state->atEof.store(true, std::memory_order_release);
            break;
        }
        if (got == 0) {
            state->atEof.store(true, std::memory_order_release);
            break;
        }

        std::vector<std::string> lines;
        assembler.feed(buf, static_cast<std::size_t>(got), lines);
        if (!lines.empty()) {
            std::lock_guard<std::mutex> lk(state->mutex);
            for (auto& line : lines)
                state->lines.push_back(std::move(line));
        }
    }

    if (state->atEof.load(std::memory_order_acquire)) {
        std::vector<std::string> lines;
        assembler.flush(lines);
        if (!lines.empty()) {
            std::lock_guard<std::mutex> lk(state->mutex);
            for (auto& line : lines)
                state->lines.push_back(std::move(line));
        }
    }

    {
        std::lock_guard<std::mutex> lk(state->finishMutex);
        state->finished.store(true, std::memory_order_release);
    }
    state->finishCv.notify_all();
}

#else

void StdinCommandReader::runReadLoop(std::shared_ptr<StdinCommandReader::State> state) {
    LineAssembler assembler;
    char buf[kReadChunkBytes];

    while (state->running.load(std::memory_order_acquire)) {
        pollfd pfd{};
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;

        const int ready = ::poll(&pfd, 1, kPollTimeoutMs);
        if (ready < 0) {
            if (errno == EINTR)
                continue; // a signal (SIGINT/SIGTERM) landed here; the main loop handles the flag
            break;
        }
        if (ready == 0)
            continue; // timed out: re-check `running` -- this is what makes stop() prompt
        if ((pfd.revents & (POLLERR | POLLNVAL)) != 0)
            break;

        const ssize_t got = ::read(STDIN_FILENO, buf, sizeof(buf));
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            break;
        }
        if (got == 0) {
            state->atEof.store(true, std::memory_order_release);
            break;
        }

        std::vector<std::string> lines;
        assembler.feed(buf, static_cast<std::size_t>(got), lines);
        if (!lines.empty()) {
            std::lock_guard<std::mutex> lk(state->mutex);
            for (auto& line : lines)
                state->lines.push_back(std::move(line));
        }
    }

    if (state->atEof.load(std::memory_order_acquire)) {
        std::vector<std::string> lines;
        assembler.flush(lines);
        if (!lines.empty()) {
            std::lock_guard<std::mutex> lk(state->mutex);
            for (auto& line : lines)
                state->lines.push_back(std::move(line));
        }
    }

    {
        std::lock_guard<std::mutex> lk(state->finishMutex);
        state->finished.store(true, std::memory_order_release);
    }
    state->finishCv.notify_all();
}

#endif

} // namespace fl
