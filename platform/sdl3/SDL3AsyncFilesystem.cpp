// SPDX-License-Identifier: GPL-3.0-or-later
#include "SDL3AsyncFilesystem.h"

#include <SDL3/SDL.h>

#include <cstddef>

SDL3AsyncFilesystem::SDL3AsyncFilesystem(std::filesystem::path assetsRoot, std::filesystem::path userDataRoot)
    : m_assetsRoot(std::move(assetsRoot)), m_userDataRoot(std::move(userDataRoot)) {}

SDL3AsyncFilesystem::~SDL3AsyncFilesystem() {
    if (m_worker.joinable())
        shutdown();
}

bool SDL3AsyncFilesystem::init() {
    if (m_initialized) {
        m_lastError = "SDL3AsyncFilesystem::init() called while already initialised";
        return false;
    }
    m_shutdown = false;
    m_initialized = true;
    m_worker = std::thread(&SDL3AsyncFilesystem::workerLoop, this);
    return true;
}

void SDL3AsyncFilesystem::shutdown() {
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

void SDL3AsyncFilesystem::setEventHandler(IAsyncFilesystemHandler* handler) {
    m_handler = handler;
}

AsyncReadId SDL3AsyncFilesystem::readFileAsync(PathDomain domain, const char* path) {
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

void SDL3AsyncFilesystem::cancelRead(AsyncReadId id) {
    auto it = m_liveRequests.find(id);
    if (it != m_liveRequests.end())
        it->second->cancelled.store(true, std::memory_order_relaxed);
}

void SDL3AsyncFilesystem::service() {
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

const char* SDL3AsyncFilesystem::getLastError() const {
    return m_lastError.empty() ? nullptr : m_lastError.c_str();
}

void SDL3AsyncFilesystem::workerLoop() {
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

        SDL_IOStream* stream = SDL_IOFromFile(fullPath.string().c_str(), "rb");
        if (!stream) {
            push({id, AsyncReadStatus::Error, {}, SDL_GetError()});
            continue;
        }

        Sint64 rawSize = SDL_GetIOSize(stream);
        if (rawSize < 0) {
            SDL_CloseIO(stream);
            push({id, AsyncReadStatus::Error, {}, SDL_GetError()});
            continue;
        }

        auto fileSize = static_cast<std::size_t>(rawSize);
        std::vector<uint8_t> data(fileSize);

        if (fileSize > 0) {
            std::size_t bytesRead = SDL_ReadIO(stream, data.data(), fileSize);
            if (bytesRead != fileSize) {
                SDL_CloseIO(stream);
                push({id, AsyncReadStatus::Error, {}, SDL_GetError()});
                continue;
            }
        }

        SDL_CloseIO(stream);

        if (req->cancelled.load(std::memory_order_relaxed)) {
            push({id, AsyncReadStatus::Cancelled, {}, {}});
        } else {
            push({id, AsyncReadStatus::Success, std::move(data), {}});
        }
    }

    // Free SDL's per-thread error buffer allocated lazily by SDL_IOFromFile.
    // Without this, LSAN reports the TLS allocation as a leak when the thread exits.
    SDL_CleanupTLS();
}
