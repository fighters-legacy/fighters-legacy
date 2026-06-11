// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/RenderSnapshot.h"

#include <atomic>
#include <cstdint>

namespace fl {

// Lock-free triple-buffer bridge between the sim thread (publisher) and the
// render thread (consumer). The sim calls publish() once per tick after all entity
// state has been updated; the render thread calls tryAdvance() once per frame to
// swap in the latest snapshot before reading current().
//
// Threading model:
//   Sim thread    — publish() only.
//   Render thread — tryAdvance(), current(), hasSnapshot() only.
//
// Three snapshot slots (indices 0, 1, 2) are maintained. At any point exactly one
// slot is owned by each of: the sim thread (m_simSlot), the "spare" (m_spare, atomic),
// and the render thread (m_renderSlot). All three indices are always a permutation of
// {0, 1, 2}; exchanging the spare with either end atomically preserves this invariant.
class SimRenderBridge {
  public:
    SimRenderBridge();

    // Sim thread: move snapshot into the write slot then atomically make it available.
    void publish(RenderSnapshot snap);

    // Main-thread only: post a snapshot received from the network when there is no
    // concurrent sim thread (network-client mode). Semantically identical to publish();
    // the threading contract is the caller's responsibility.
    void publishExternal(RenderSnapshot snap);

    // Render thread: swap the render slot with the latest published snapshot if one
    // is available since the last call. Returns true when a newer snapshot was swapped in.
    bool tryAdvance() noexcept;

    // Render thread: read the current snapshot (valid after the first successful tryAdvance).
    [[nodiscard]] const RenderSnapshot& current() const noexcept;

    // Render thread: true once at least one snapshot has been published.
    [[nodiscard]] bool hasSnapshot() const noexcept;

    // Reset all three slots to empty and clear the publish/consume counters.
    // Call from the main thread when no sim thread or render loop is running
    // (e.g. stopGame()) so hasSnapshot() returns false again for the next session.
    void reset() noexcept;

  private:
    RenderSnapshot m_snaps[3];

    // Index of the "spare" slot — the most recently completed snapshot waiting for
    // the render thread to consume. Always a valid index (0, 1, or 2).
    std::atomic<int> m_spare{2};

    // Incremented by publish(); render thread checks against m_consumeCount to decide
    // whether to call exchange. Acquire/release ordered with the spare exchange.
    std::atomic<uint64_t> m_publishCount{0};

    int m_simSlot{0};           // sim-thread-local write slot
    int m_renderSlot{1};        // render-thread-local read slot
    uint64_t m_consumeCount{0}; // render-thread-local mirror of m_publishCount
};

} // namespace fl
