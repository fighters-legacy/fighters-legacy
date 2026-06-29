// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace fl {

// Resolve the number of *background* worker threads to spawn for a JobSystem.
//
// `requested` is the JobSystem constructor argument = desired TOTAL parallelism, including the
// calling thread (which always participates in a batch):
//   1    -> 0 background  (serial / inline).
//   N>=2 -> N-1 background (N total).
//   0    -> auto: (detected>0?detected:4) - 1, i.e. leave the calling thread one core's worth.
//           A single-core host (detected<=1) degenerates to inline (0 background).
// `detected` is std::thread::hardware_concurrency() (may be 0 when the platform can't report it).
//
// Pure function (no threads) so the auto/fallback branches are unit-testable directly.
[[nodiscard]] unsigned resolveWorkerCount(unsigned requested, unsigned detected) noexcept;

// Persistent worker pool with a blocking parallel_for.
//
// Threading model:
//   - A single owner thread drives parallel_for() (the sim thread in production). It is NOT
//     reentrant and must not be called concurrently from multiple threads.
//   - The owner thread PARTICIPATES in every batch, so a JobSystem(1) (no background workers)
//     executes everything inline with zero thread/synchronisation overhead.
//   - Background workers block on a condition_variable while idle (no busy-spin) so they do not
//     steal CPU between sim ticks.
class JobSystem {
  public:
    // workerCount = desired total parallelism including the calling thread.
    // 0 = auto (from hardware_concurrency), 1 = serial/inline. See resolveWorkerCount().
    explicit JobSystem(unsigned workerCount = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;

    // Split [0, count) into chunks of up to `grain` indices, claimed dynamically by the workers
    // and the calling thread, invoking fn(begin, end) on each chunk. Blocks until every chunk
    // completes. If any chunk throws, the remaining chunks still run and the first captured
    // exception is rethrown on the calling thread.
    template <typename Fn> void parallel_for(std::size_t count, std::size_t grain, Fn&& fn) {
        if (count == 0)
            return;
        if (grain == 0)
            grain = 1;
        if (m_workers.empty()) {
            // Inline fast path: no background workers, no type erasure, exceptions propagate.
            fn(std::size_t{0}, count);
            return;
        }
        dispatch(count, grain, std::function<void(std::size_t, std::size_t)>(std::forward<Fn>(fn)));
    }

    // Number of background worker threads (0 = serial/inline path).
    [[nodiscard]] unsigned workerCount() const noexcept {
        return static_cast<unsigned>(m_workers.size());
    }

  private:
    void dispatch(std::size_t count, std::size_t grain, std::function<void(std::size_t, std::size_t)> fn);
    void workerLoop();
    void runChunks(); // claim and run chunks of the current batch (workers + caller)

    std::vector<std::thread> m_workers;

    std::mutex m_mx;
    std::condition_variable m_workCv; // workers wait here for a new batch
    std::condition_variable m_doneCv; // the caller waits here for batch completion

    std::function<void(std::size_t, std::size_t)> m_fn;
    std::size_t m_count{0};
    std::size_t m_grain{1};
    std::atomic<std::size_t> m_cursor{0};
    std::uint64_t m_batchGen{0};
    std::size_t m_workersActive{0};
    bool m_stop{false};
    std::exception_ptr m_exc;
};

} // namespace fl
