// SPDX-License-Identifier: GPL-3.0-or-later
#include "AsyncWriter.h"

#include <ILogger.h>

#include <cstdio>
#include <utility>

namespace fl::persist {

AsyncWriter::AsyncWriter(std::size_t maxQueue, ILogger* log) : mMaxQueue(maxQueue == 0 ? 1 : maxQueue), mLog(log) {}

AsyncWriter::~AsyncWriter() {
    stop();
}

void AsyncWriter::start() {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mStarted)
        return;
    mStarted = true;
    mStopping = false;
    mThread = std::thread([this] { run(); });
}

void AsyncWriter::enqueue(Task task) {
    std::unique_lock<std::mutex> lock(mMutex);
    if (mStopping || !mStarted) {
        // Shutdown, or a backend that failed to open. Either way the caller is past the point where
        // this write could matter -- but say so, because "the store silently stopped accepting
        // writes" is the failure mode this subsystem is built to make impossible.
        if (mLog)
            mLog->log(LogLevel::Warn, __FILE__, __LINE__, "persistence: write discarded -- the store is closed");
        return;
    }
    if (mQueue.size() >= mMaxQueue && !mWarnedFull) {
        mWarnedFull = true;
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "persistence: write queue full at %zu; callers will block until it drains. "
                      "Raise [persistence] write_queue_max if this repeats.",
                      mMaxQueue);
        if (mLog)
            mLog->log(LogLevel::Warn, __FILE__, __LINE__, buf);
    }
    mDrain.wait(lock, [this] { return mQueue.size() < mMaxQueue || mStopping; });
    if (mStopping)
        return;

    mQueue.push_back(std::move(task));
    ++mEnqueued;
    if (mQueue.size() > mHighWater)
        mHighWater = mQueue.size();
    mWork.notify_one();
}

Result AsyncWriter::flush() {
    std::unique_lock<std::mutex> lock(mMutex);
    // Wait for the queue to empty AND for the in-flight task to finish. Waiting on the queue alone
    // returns while the last write is still running, which is exactly the moment a shutdown path or
    // a test would then read a store that has not caught up.
    mDrain.wait(lock, [this] { return (mQueue.empty() && !mBusy) || !mStarted; });
    Result r = mFlushWindowError.empty() ? Result::success() : Result::failure(mFlushWindowError);
    mFlushWindowError.clear();
    return r;
}

void AsyncWriter::stop() {
    std::thread thread;
    {
        std::unique_lock<std::mutex> lock(mMutex);
        if (!mStarted)
            return;
        // Drain first: stopping is a shutdown, and shutdown is precisely when the queued writes
        // matter most. Only then do we refuse new work.
        mDrain.wait(lock, [this] { return mQueue.empty() && !mBusy; });
        mStopping = true;
        mWork.notify_all();
        mDrain.notify_all();
        thread = std::move(mThread);
        mStarted = false;
    }
    if (thread.joinable())
        thread.join();
}

void AsyncWriter::run() {
    std::unique_lock<std::mutex> lock(mMutex);
    for (;;) {
        mWork.wait(lock, [this] { return !mQueue.empty() || mStopping; });
        if (mQueue.empty()) {
            if (mStopping)
                return;
            continue;
        }
        Task task = std::move(mQueue.front());
        mQueue.pop_front();
        mBusy = true;
        // Room freed: a blocked enqueue can proceed while this task runs.
        mDrain.notify_all();

        Result result;
        lock.unlock();
        result = task();
        lock.lock();

        mBusy = false;
        if (result) {
            ++mCompleted;
        } else {
            ++mFailed;
            mLastError = result.error;
            if (mFlushWindowError.empty())
                mFlushWindowError = result.error;
            if (mLog) {
                char buf[512];
                std::snprintf(buf, sizeof(buf), "persistence: write failed: %s", result.error.c_str());
                mLog->log(LogLevel::Error, __FILE__, __LINE__, buf);
            }
        }
        mDrain.notify_all();
    }
}

AsyncWriter::Stats AsyncWriter::stats() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return Stats{mEnqueued, mCompleted, mFailed, mQueue.size(), mHighWater, mLastError};
}

} // namespace fl::persist
