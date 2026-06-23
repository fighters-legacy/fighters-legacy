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

class SDL3AsyncFilesystem : public IAsyncFilesystem {
  public:
    explicit SDL3AsyncFilesystem(std::filesystem::path assetsRoot, std::filesystem::path userDataRoot);
    ~SDL3AsyncFilesystem() override;

    SDL3AsyncFilesystem(const SDL3AsyncFilesystem&) = delete;
    SDL3AsyncFilesystem& operator=(const SDL3AsyncFilesystem&) = delete;

    bool init() override;
    void shutdown() override;
    void setEventHandler(IAsyncFilesystemHandler* handler) override;
    AsyncReadId readFileAsync(PathDomain domain, const char* path) override;
    void cancelRead(AsyncReadId id) override;
    void service() override;
    const char* getLastError() const override;

  private:
    struct PendingRequest {
        AsyncReadId id;
        PathDomain domain;
        std::string path;
        std::atomic<bool> cancelled{false};
    };

    struct CompletedRequest {
        AsyncReadId id;
        AsyncReadStatus status;
        std::vector<uint8_t> data;
        std::string errorMsg;
    };

    void workerLoop();

    std::filesystem::path m_assetsRoot;
    std::filesystem::path m_userDataRoot;

    uint32_t m_nextId{1};      // main-thread only; skip 0 on rollover
    bool m_initialized{false}; // set by init(), cleared by shutdown()

    IAsyncFilesystemHandler* m_handler{nullptr}; // main-thread only
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
