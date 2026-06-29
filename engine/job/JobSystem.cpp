// SPDX-License-Identifier: GPL-3.0-or-later
#include "job/JobSystem.h"

#include <algorithm>

static_assert(std::atomic<std::size_t>::is_always_lock_free, "size_t atomic must be lock-free on this platform");

namespace fl {

unsigned resolveWorkerCount(unsigned requested, unsigned detected) noexcept {
    if (requested == 1u)
        return 0u; // explicit serial / inline
    if (requested >= 2u)
        return requested - 1u; // N total parallelism -> N-1 background workers
    // requested == 0 -> auto: leave the calling thread one core's worth.
    const unsigned d = (detected == 0u) ? 4u : detected; // fall back when undetectable
    return (d > 1u) ? (d - 1u) : 0u;
}

JobSystem::JobSystem(unsigned workerCount) {
    const unsigned n = resolveWorkerCount(workerCount, std::thread::hardware_concurrency());
    m_workers.reserve(n);
    for (unsigned i = 0; i < n; ++i)
        m_workers.emplace_back([this] { workerLoop(); });
}

JobSystem::~JobSystem() {
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_stop = true;
    }
    m_workCv.notify_all();
    for (auto& t : m_workers)
        if (t.joinable())
            t.join();
}

void JobSystem::runChunks() {
    for (;;) {
        const std::size_t begin = m_cursor.fetch_add(m_grain, std::memory_order_relaxed);
        if (begin >= m_count)
            break;
        const std::size_t end = std::min(begin + m_grain, m_count);
        try {
            m_fn(begin, end);
        } catch (...) {
            std::lock_guard<std::mutex> lk(m_mx);
            if (!m_exc)
                m_exc = std::current_exception();
        }
    }
}

void JobSystem::workerLoop() {
    std::uint64_t lastGen = 0;
    for (;;) {
        std::unique_lock<std::mutex> lk(m_mx);
        m_workCv.wait(lk, [&] { return m_stop || m_batchGen != lastGen; });
        if (m_stop)
            return;
        lastGen = m_batchGen;
        lk.unlock();

        runChunks();

        lk.lock();
        if (--m_workersActive == 0)
            m_doneCv.notify_one();
    }
}

void JobSystem::dispatch(std::size_t count, std::size_t grain, std::function<void(std::size_t, std::size_t)> fn) {
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_fn = std::move(fn);
        m_count = count;
        m_grain = grain;
        m_cursor.store(0, std::memory_order_relaxed);
        m_exc = nullptr;
        m_workersActive = m_workers.size();
        ++m_batchGen;
    }
    m_workCv.notify_all();

    // The calling thread participates in the batch.
    runChunks();

    std::exception_ptr exc;
    {
        std::unique_lock<std::mutex> lk(m_mx);
        m_doneCv.wait(lk, [&] { return m_workersActive == 0; });
        exc = m_exc;
        m_exc = nullptr;
        m_fn = nullptr; // release captured batch state
    }
    if (exc)
        std::rethrow_exception(exc);
}

} // namespace fl
