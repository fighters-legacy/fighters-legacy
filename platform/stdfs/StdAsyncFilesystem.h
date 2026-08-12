// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IAsyncFilesystem.h"
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fl {

// std::ifstream backed IAsyncFilesystem: a single worker thread reads whole
// files off a mutex/condvar queue; completions are swap-drained on the main
// thread in service(). No windowing/library dependency (unlike the former SDL3
// backend). Callers pass in already-resolved assets/user-data roots.
class StdAsyncFilesystem : public IAsyncFilesystem {
  public:
    explicit StdAsyncFilesystem(std::filesystem::path assetsRoot, std::filesystem::path userDataRoot);
    ~StdAsyncFilesystem() override;

    StdAsyncFilesystem(const StdAsyncFilesystem&) = delete;
    StdAsyncFilesystem& operator=(const StdAsyncFilesystem&) = delete;

    bool init() override;
    void shutdown() override;
    AsyncReadId readFileAsync(PathDomain domain, const char* path, IAsyncFilesystemHandler* handler) override;
    void cancelRead(AsyncReadId id) override;
    void cancelReadsFor(IAsyncFilesystemHandler* handler) override;
    void service() override;
    const char* getLastError() const override;

  private:
    struct PendingRequest {
        AsyncReadId id;
        PathDomain domain;
        std::string path;
        std::atomic<bool> cancelled{false};
        // Who this read's completion belongs to (#1083). Written on the main thread before the worker
        // sees the request and read on the main thread in service(); the worker never touches it.
        IAsyncFilesystemHandler* handler{nullptr};
    };

    struct CompletedRequest {
        AsyncReadId id;
        AsyncReadStatus status;
        std::vector<uint8_t> data;
        std::string errorMsg;
        // Carried through with the completion so service() can deliver it without a second lookup, and
        // so a completion queued by shutdown() still knows where it belongs.
        IAsyncFilesystemHandler* handler{nullptr};
    };

    void workerLoop();

    std::filesystem::path m_assetsRoot;
    std::filesystem::path m_userDataRoot;

    uint32_t m_nextId{1};      // main-thread only; skip 0 on rollover
    bool m_initialized{false}; // set by init(), cleared by shutdown()

    mutable std::string m_lastError;

    std::thread m_worker;
    std::mutex m_queueMtx;
    std::condition_variable m_cv;
    bool m_shutdown{false}; // set+read under m_queueMtx

    std::queue<std::shared_ptr<PendingRequest>> m_pendingQueue;                      // guarded by m_queueMtx
    std::unordered_map<AsyncReadId, std::shared_ptr<PendingRequest>> m_liveRequests; // main-thread only

    std::mutex m_completedMtx;
    std::vector<CompletedRequest> m_completedQueue; // swap-drain in service()
};

} // namespace fl
