// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SimThreadOwnership (#1094) — the process's sim-thread identity, for debug-build ownership asserts.
//
// Several engine classes document a threading contract in a header comment and enforce it with
// nothing. `FactionRegistry` is the sharpest case: load-once state read lock-free from any thread,
// relationships mutated sim-thread-only with no lock, and alert levels mutex-guarded because they
// arrive from the network or main thread — three tiers, mixed locked and unlocked members in one
// class, and the rule held by prose. That is the kind of contract ThreadSanitizer finds at 128
// players and never at 8: a sim-thread-only write from an admin command shows up as a corrupted
// faction table under load, long after the change that caused it.
//
// So the contract gets a mechanism. This is deliberately the smallest one that can express it: there
// is exactly ONE sim thread per process (GameLoop owns it), so its identity is process state, and a
// class asking "am I on the sim thread?" needs no reference to GameLoop to find out. Header-only and
// stdlib-only, with function-local statics, so a consumer gains no link edge — the `Capability.h`
// pattern: a vocabulary header shared across a layer boundary at zero link cost. `engine-world` must
// not acquire a dependency on `engine-loop` to check a thread id.
//
// The three states matter, and "no sim thread" is not the same as "wrong thread":
//
//   * BEFORE the sim thread starts — single-threaded init. Every tier is writable; an assertion that
//     fired here would condemn `load()`, which is specified to run before `start()`.
//   * WHILE it runs — sim-thread-only members may be touched only from that thread.
//   * AFTER it stops — teardown, single-threaded again.
//
// Assertions built on this are `assert()`, so they compile out of release builds entirely. The
// claim/release calls do not: they are two relaxed atomic stores per sim-thread lifetime, and state
// that exists only in debug builds is state nobody can inspect when a release build misbehaves.

#include <atomic>
#include <thread>

namespace fl {

class SimThreadOwnership {
  public:
    // Called by GameLoop from the sim thread itself, as its first act.
    static void claim() noexcept {
        ownerId().store(std::this_thread::get_id(), std::memory_order_relaxed);
        active().store(true, std::memory_order_release);
    }

    // Called by GameLoop as the sim thread's last act.
    static void release() noexcept {
        active().store(false, std::memory_order_release);
    }

    // True between claim() and release(): a sim thread exists and owns its tier.
    [[nodiscard]] static bool simThreadActive() noexcept {
        return active().load(std::memory_order_acquire);
    }

    [[nodiscard]] static bool onSimThread() noexcept {
        return simThreadActive() && ownerId().load(std::memory_order_relaxed) == std::this_thread::get_id();
    }

    // The predicate a sim-thread-only member asserts on: either no sim thread exists (init or
    // teardown, single-threaded) or this IS it. Reads as the contract rather than as its mechanics.
    [[nodiscard]] static bool onSimThreadOrSingleThreaded() noexcept {
        return !simThreadActive() || onSimThread();
    }

  private:
    static std::atomic<std::thread::id>& ownerId() noexcept {
        static std::atomic<std::thread::id> value{};
        return value;
    }
    static std::atomic<bool>& active() noexcept {
        static std::atomic<bool> value{false};
        return value;
    }
};

} // namespace fl
