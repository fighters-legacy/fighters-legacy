// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/SimRenderBridge.h"

static_assert(std::atomic<int>::is_always_lock_free, "SimRenderBridge requires lock-free int atomics");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "SimRenderBridge requires lock-free uint64_t atomics");

namespace fl {

SimRenderBridge::SimRenderBridge() {
    // Initial slot ownership: sim=0, spare=2, render=1 — all distinct.
    // m_spare{2} and m_simSlot{0} / m_renderSlot{1} are set by member initializers.
}

void SimRenderBridge::publish(RenderSnapshot snap) {
    m_snaps[m_simSlot] = std::move(snap);
    // Atomically swap the completed sim slot into the spare; reclaim the old spare as
    // the next write slot. The release fence ensures the snapshot data written above is
    // visible to the render thread after it acquires through tryAdvance().
    int old = m_spare.exchange(m_simSlot, std::memory_order_release);
    m_simSlot = old;
    m_publishCount.fetch_add(1, std::memory_order_release);
}

void SimRenderBridge::publishExternal(RenderSnapshot snap) {
    // Identical implementation to publish(); the caller guarantees no concurrent
    // sim thread is running (network-client mode with no local GameLoop).
    m_snaps[m_simSlot] = std::move(snap);
    int old = m_spare.exchange(m_simSlot, std::memory_order_release);
    m_simSlot = old;
    m_publishCount.fetch_add(1, std::memory_order_release);
}

bool SimRenderBridge::tryAdvance() noexcept {
    uint64_t count = m_publishCount.load(std::memory_order_acquire);
    if (count == m_consumeCount)
        return false;
    // A newer snapshot waits in the spare. Swap render slot with spare; the acq_rel
    // fence pairs with the release in publish() so the snapshot contents are visible.
    int spare = m_spare.exchange(m_renderSlot, std::memory_order_acq_rel);
    m_renderSlot = spare;
    m_consumeCount = count;
    return true;
}

const RenderSnapshot& SimRenderBridge::current() const noexcept {
    return m_snaps[m_renderSlot];
}

bool SimRenderBridge::hasSnapshot() const noexcept {
    return m_publishCount.load(std::memory_order_relaxed) > 0;
}

void SimRenderBridge::reset() noexcept {
    for (auto& s : m_snaps)
        s = RenderSnapshot{};
    m_spare.store(2, std::memory_order_relaxed);
    m_publishCount.store(0, std::memory_order_relaxed);
    m_simSlot = 0;
    m_renderSlot = 1;
    m_consumeCount = 0;
}

} // namespace fl
