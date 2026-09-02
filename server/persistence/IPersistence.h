// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The server-side persistence store (#533, plan #1366 decision D24).
//
// WHAT THIS IS, AND WHAT IT DELIBERATELY IS NOT. It is a server-side library with TYPED
// repositories, not a platform HAL and not a key-value store. Nothing in engine/ consumes it —
// the campaign blob is handed to ServerRuntime already — so it lives under server/ as its own
// target, and cmake/layering.cmake pins that: no engine-* target may reach a SQL engine. A generic
// `get(key)/put(key, bytes)` façade was the obvious alternative and is the wrong one; it moves
// every schema decision into call sites where nothing can check them, which is exactly how the
// two hand-rolled stores this replaces (banlist.txt, cache/*.flsave) drifted apart in the first
// place.
//
// THREADING CONTRACT. Three tiers, and the first is the one that matters:
//
//   1. The SIM THREAD NEVER TOUCHES THE STORE. Not for a read, not for a write. A sim-originated
//      write reaches it the way ban persistence already does today — an enqueueSimCallback hop to
//      the main thread, and the store call happens there. This is the generalization of the
//      existing `saveBanlist` reasoning, and it is the whole reason the writer below is async.
//   2. WRITES are enqueued and applied on one writer thread that owns the write connection. A
//      write call returns once the work is QUEUED, not once it is durable — flush() is how a
//      caller waits for durability, and shutdown does exactly that.
//   3. READS are synchronous on the calling thread (main or admin) and are safe from several
//      threads at once.
//
// ERRORS ARE NEVER SWALLOWED. An async write cannot return a status to its caller, so the failure
// path is: log at Error, and count it in StoreHealth::writesFailed, which the admin surface
// reports. A store that quietly stops persisting while the operator believes their bans are
// durable is the exact failure this subsystem exists to prevent, so "the write failed and nobody
// will ever know" must not be reachable.
//
// C++20 here, so there is no std::expected. Reads answer std::optional and writes answer Result —
// the log-and-report idiom the surrounding server code already uses. No exceptions cross this
// seam.

#include "Repositories.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fl::persist {

// What the store knows about itself. Read on any thread; the counters are atomics behind the
// implementation, so a reader sees a consistent-enough snapshot for an operator report.
struct StoreHealth {
    bool open{false};
    int schemaVersion{0};
    // Writes accepted, applied, and failed since the process started. writesFailed is the number
    // an operator actually needs: it is non-zero only when the store has silently stopped doing
    // its job.
    std::uint64_t writesEnqueued{0};
    std::uint64_t writesCompleted{0};
    std::uint64_t writesFailed{0};
    // Current and worst-seen queue depth. The high-water mark is the honest capacity signal: a
    // queue that reaches its cap once under load is a configuration to raise, and a depth sampled
    // at a quiet moment would never show it.
    std::size_t queueDepth{0};
    std::size_t queueHighWater{0};
    // The last write error, kept verbatim. Empty when writesFailed is 0.
    std::string lastError;
};

class IPersistence {
  public:
    virtual ~IPersistence() = default;

    // The typed repositories. #533 shipped blobs — the schema-trivial one, which proved the whole
    // vertical (dependency, migration runner, repository, async writer, two backends) before the
    // schema existed. #534 adds the other three on that proven machinery.
    //
    // Their live producers arrive later and separately: #535 swaps the ban seam onto bans(), and
    // #929 feeds stats() once identity gives it a verified account to key on. Both repositories are
    // implemented and tested here regardless — the alternative was declaring interfaces nothing
    // implements, which is what #533 refused to do.
    [[nodiscard]] virtual IBlobRepository& blobs() = 0;
    [[nodiscard]] virtual IAccountRepository& accounts() = 0;
    [[nodiscard]] virtual IBanRepository& bans() = 0;
    [[nodiscard]] virtual IStatsRepository& stats() = 0;

    // Block until every write enqueued before this call has been applied. Returns the first error
    // seen since the previous flush, so a caller that wants durability (shutdown, a test, a
    // migration handoff) gets one and can act on it.
    virtual Result flush() = 0;

    // Drain, stop the writer thread, and close the connections. Idempotent. Called on the shutdown
    // path AFTER the last write, and by the destructor if a caller forgets.
    virtual void close() = 0;

    [[nodiscard]] virtual StoreHealth health() const = 0;

    // "sqlite" / "postgres" / "null" — what the operator sees in `status`, so the answer to "is
    // this server persisting anything?" never has to be inferred from config.
    [[nodiscard]] virtual std::string_view backendName() const = 0;
};

} // namespace fl::persist
