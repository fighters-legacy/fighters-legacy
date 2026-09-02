// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The store's write thread (#533, D24).
//
// One thread, one queue, one write connection. Every backend uses it, so the "the sim thread never
// blocks on the store" property is a property of the subsystem rather than something each backend
// remembers to implement.
//
// FULL QUEUE BLOCKS; IT DOES NOT DROP. The alternative — discard the task, count it, carry on — is
// how a ban disappears on a busy server and nobody finds out until the banned player is back. The
// callers are the main and admin threads (never the sim thread, by the contract in IPersistence.h),
// so blocking one of them briefly under a write storm is a latency cost on an already-degraded
// server, which is the right thing to spend. The first block logs a Warn naming the cap, so an
// operator who hits it learns the config key to raise.

#include "Repositories.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace fl {
class ILogger;
}

namespace fl::persist {

class AsyncWriter {
  public:
    // A queued unit of work, run on the writer thread. It captures whatever it needs by value —
    // the caller's stack is long gone by the time it runs.
    using Task = std::function<Result()>;

    AsyncWriter(std::size_t maxQueue, ILogger* log);
    ~AsyncWriter();

    AsyncWriter(const AsyncWriter&) = delete;
    AsyncWriter& operator=(const AsyncWriter&) = delete;

    // Start the thread. Separate from the constructor so a backend can run its migrations on the
    // calling thread first, with nothing else touching the connection.
    void start();

    // Enqueue work. Blocks while the queue is at its cap. Returns immediately if the writer has
    // been stopped, dropping the task — but a stopped writer means the server is shutting down and
    // the caller is past the point where the write mattered, and that path logs.
    void enqueue(Task task);

    // Block until everything enqueued before this call has run. Returns the first error since the
    // last flush and clears it, so each flush reports its own window rather than the same failure
    // forever.
    Result flush();

    // Drain the queue, then stop and join. Idempotent.
    void stop();

    struct Stats {
        std::uint64_t enqueued{0};
        std::uint64_t completed{0};
        std::uint64_t failed{0};
        std::size_t depth{0};
        std::size_t highWater{0};
        std::string lastError;
    };
    [[nodiscard]] Stats stats() const;

  private:
    void run();

    const std::size_t mMaxQueue;
    ILogger* mLog{nullptr};

    mutable std::mutex mMutex;
    std::condition_variable mWork;  // writer waits for a task or a stop
    std::condition_variable mDrain; // enqueue waits for room; flush waits for idle
    std::deque<Task> mQueue;
    bool mStopping{false};
    bool mBusy{false}; // a task is running right now: flush must wait for it, not just for an empty queue

    std::thread mThread;
    bool mStarted{false};

    // Counters. Guarded by mMutex rather than atomic: they are always touched with the lock already
    // held, and a torn read across six fields would make an operator report self-inconsistent.
    std::uint64_t mEnqueued{0};
    std::uint64_t mCompleted{0};
    std::uint64_t mFailed{0};
    std::size_t mHighWater{0};
    std::string mLastError;        // survives flush() for the operator report
    std::string mFlushWindowError; // cleared by flush(), which returns it
    bool mWarnedFull{false};
};

} // namespace fl::persist
