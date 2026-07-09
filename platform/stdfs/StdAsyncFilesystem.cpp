// SPDX-License-Identifier: GPL-3.0-or-later
#include "StdAsyncFilesystem.h"

#include <cstddef>
#include <fstream>
#include <system_error>

namespace fl {

StdAsyncFilesystem::StdAsyncFilesystem(std::filesystem::path assetsRoot, std::filesystem::path userDataRoot)
    : m_assetsRoot(std::move(assetsRoot)), m_userDataRoot(std::move(userDataRoot)) {}

StdAsyncFilesystem::~StdAsyncFilesystem() {
    if (m_worker.joinable())
        shutdown();
}

bool StdAsyncFilesystem::init() {
    if (m_initialized) {
        m_lastError = "StdAsyncFilesystem::init() called while already initialised";
        return false;
    }
    m_shutdown = false;
    m_initialized = true;
    m_worker = std::thread(&StdAsyncFilesystem::workerLoop, this);
    return true;
}

void StdAsyncFilesystem::shutdown() {
    // Mark all live requests as cancelled so the worker skips or discards them.
    for (auto& [id, req] : m_liveRequests)
        req->cancelled.store(true, std::memory_order_relaxed);

    {
        std::lock_guard lock(m_queueMtx);
        m_shutdown = true;
    }
    m_cv.notify_all();

    if (m_worker.joinable())
        m_worker.join();

    // Drain any completions that arrived between the join and now.
    service();

    m_liveRequests.clear();
    m_initialized = false;
}

void StdAsyncFilesystem::setEventHandler(IAsyncFilesystemHandler* handler) {
    m_handler = handler;
}

AsyncReadId StdAsyncFilesystem::readFileAsync(PathDomain domain, const char* path) {
    if (!m_initialized || !path)
        return 0;

    uint32_t id = m_nextId++;
    if (m_nextId == 0)
        m_nextId = 1;

    auto req = std::make_shared<PendingRequest>();
    req->id = id;
    req->domain = domain;
    req->path = path;

    m_liveRequests[id] = req;

    {
        std::lock_guard lock(m_queueMtx);
        m_pendingQueue.push(req);
    }
    m_cv.notify_one();
    return id;
}

void StdAsyncFilesystem::cancelRead(AsyncReadId id) {
    auto it = m_liveRequests.find(id);
    if (it != m_liveRequests.end())
        it->second->cancelled.store(true, std::memory_order_relaxed);
}

void StdAsyncFilesystem::service() {
    std::vector<CompletedRequest> batch;
    {
        std::lock_guard lock(m_completedMtx);
        std::swap(batch, m_completedQueue);
    }
    for (auto& c : batch) {
        m_liveRequests.erase(c.id);
        if (m_handler)
            m_handler->onReadComplete(c.id, c.status, c.data.empty() ? nullptr : c.data.data(), c.data.size(),
                                      c.errorMsg.empty() ? nullptr : c.errorMsg.c_str());
    }
}

const char* StdAsyncFilesystem::getLastError() const {
    return m_lastError.empty() ? nullptr : m_lastError.c_str();
}

void StdAsyncFilesystem::workerLoop() {
    auto push = [&](CompletedRequest r) {
        std::lock_guard lock(m_completedMtx);
        m_completedQueue.push_back(std::move(r));
    };

    while (true) {
        std::shared_ptr<PendingRequest> req;
        {
            std::unique_lock lock(m_queueMtx);
            m_cv.wait(lock, [&] { return !m_pendingQueue.empty() || m_shutdown; });
            if (m_shutdown && m_pendingQueue.empty())
                break;
            req = std::move(m_pendingQueue.front());
            m_pendingQueue.pop();
        }

        AsyncReadId id = req->id;

        if (req->cancelled.load(std::memory_order_relaxed)) {
            push({id, AsyncReadStatus::Cancelled, {}, {}});
            continue;
        }

        const std::filesystem::path base = (req->domain == PathDomain::Assets) ? m_assetsRoot : m_userDataRoot;
        const std::filesystem::path fullPath = base / req->path;

        // Open from the fs::path object so Windows uses the wide native path
        // (UTF-8-safe). std::ifstream needs no per-thread TLS cleanup.
        std::ifstream ifs(fullPath, std::ios::binary);
        if (!ifs) {
            push({id, AsyncReadStatus::Error, {}, "failed to open file: " + fullPath.string()});
            continue;
        }

        std::error_code ec;
        std::uintmax_t rawSize = std::filesystem::file_size(fullPath, ec);
        if (ec) {
            push({id, AsyncReadStatus::Error, {}, ec.message()});
            continue;
        }

        auto fileSize = static_cast<std::size_t>(rawSize);
        std::vector<uint8_t> data(fileSize);

        if (fileSize > 0) {
            ifs.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(fileSize));
            if (static_cast<std::size_t>(ifs.gcount()) != fileSize) {
                push({id, AsyncReadStatus::Error, {}, "short read: " + fullPath.string()});
                continue;
            }
        }

        if (req->cancelled.load(std::memory_order_relaxed)) {
            push({id, AsyncReadStatus::Cancelled, {}, {}});
        } else {
            push({id, AsyncReadStatus::Success, std::move(data), {}});
        }
    }
}

} // namespace fl
