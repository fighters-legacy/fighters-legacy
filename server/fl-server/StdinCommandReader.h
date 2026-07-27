// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// StdinCommandReader (#1038) — the admin console's stdin line source.
//
// This exists because of a real deadlock. fl-server used to read stdin on a DETACHED thread blocked
// in `std::getline(std::cin, line)`. A blocked getline holds the C-stdio lock for `stdin`, and
// `exit()` runs `_IO_cleanup`, which walks every open FILE to flush it and blocks trying to take
// that same lock. Nothing could wake the reader (it was inside a blocking `read()`) and nothing
// joined it, so the process hung after `main()` returned -- on BOTH `quit` and Ctrl-C, for every
// parent that keeps stdin open (an interactive terminal, `docker run -i`, a supervisor). Only a
// stdin that reached EOF exited cleanly, which is why every piped smoke test missed it.
//
// Two properties fix it, and both matter:
//
//   1. The reader waits with a TIMEOUT and re-checks a running flag, so `stop()` can end it and
//      join before `main()` returns. This is the RconServer poll-loop shape, not a new invention.
//   2. The reader touches the raw OS handle (fd 0 / STD_INPUT_HANDLE) and never C stdio. It cannot
//      hold the lock that exit-time flushing waits on -- which is what made the old code a deadlock
//      rather than merely a thread that outlived its usefulness.
//
// Shared state is held behind a shared_ptr the worker captures by value, so the ONE case where the
// thread cannot be joined promptly (a Windows console read already inside `ReadFile` on a
// half-typed line) degrades to a detach that is memory-safe and, by property 2, exit-safe.

namespace fl {

// Pure incremental line splitter: feed arbitrary byte chunks, get complete lines out. Split from
// the reader so the interesting logic (partial lines across reads, CRLF, overlong input) is
// unit-testable without a file descriptor, a thread, or a platform.
class LineAssembler {
  public:
    // A console command line longer than this is not a command, it is an accident or an attack.
    // The overlong prefix is truncated to the cap and the rest of that line is discarded, rather
    // than letting one unterminated stream grow the buffer without bound.
    static constexpr std::size_t kMaxLineBytes = 64u * 1024u;

    // Append `len` bytes; every completed line (terminator stripped, trailing CR stripped) is
    // pushed onto `out` in arrival order.
    void feed(const char* data, std::size_t len, std::vector<std::string>& out);

    // End of input: emit a trailing unterminated line, if any. Idempotent.
    void flush(std::vector<std::string>& out);

    [[nodiscard]] std::size_t pendingBytes() const noexcept {
        return m_partial.size();
    }

  private:
    std::string m_partial;
    bool m_overlong{false}; // discarding the tail of a line that blew past kMaxLineBytes
};

class StdinCommandReader {
  public:
    // Poll wait per iteration. Bounds how long `stop()` waits for the worker to notice.
    static constexpr int kPollTimeoutMs = 100;
    // How long `stop()` waits for the worker to finish before detaching instead of joining.
    static constexpr int kJoinTimeoutMs = 250;

    StdinCommandReader() = default;
    ~StdinCommandReader();

    StdinCommandReader(const StdinCommandReader&) = delete;
    StdinCommandReader& operator=(const StdinCommandReader&) = delete;

    // Launch the reader thread. Idempotent -- a second call while running is a no-op.
    void start();

    // Move every line received since the last call into `out` (appended, oldest first).
    // Any thread; returns immediately. Safe before start() and after stop() (yields nothing new).
    void drain(std::vector<std::string>& out);

    // True once stdin reached EOF (a closed pipe, `printf ... | fl-server`). The server does not
    // exit on EOF -- it simply stops receiving console commands, which is the historical behaviour.
    [[nodiscard]] bool eof() const noexcept;

    // Stop reading and release the thread. Idempotent; safe if start() was never called.
    void stop();

  private:
    struct State {
        std::mutex mutex; // guards `lines`
        std::vector<std::string> lines;
        std::atomic<bool> running{true};
        std::atomic<bool> finished{false};
        std::atomic<bool> atEof{false};
        std::mutex finishMutex;
        std::condition_variable finishCv;
    };

    static void runReadLoop(std::shared_ptr<State> state); // the worker body; platform code lives here

    std::shared_ptr<State> m_state;
    std::thread m_thread;
};

} // namespace fl
