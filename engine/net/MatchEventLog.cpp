// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/MatchEventLog.h"

#include <algorithm>

namespace fl {

MatchEventLog::MatchEventLog(std::size_t capacity) : m_capacity(capacity == 0 ? 1 : capacity) {
    // Allocated once, up front: a ring that grows would defeat the bound it exists to enforce, and
    // the sim thread must not allocate mid-tick to record a kill.
    m_ring.resize(m_capacity);
}

void MatchEventLog::append(MatchEvent ev) {
    std::lock_guard<std::mutex> lk(m_mutex);
    ev.seq = m_nextSeq++;
    if (m_count == m_capacity)
        ++m_dropped; // overwriting the oldest retained record
    m_ring[m_head] = std::move(ev);
    m_head = (m_head + 1) % m_capacity;
    if (m_count < m_capacity)
        ++m_count;
}

std::vector<MatchEvent> MatchEventLog::since(uint64_t afterSeq) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<MatchEvent> out;
    if (m_count == 0)
        return out;

    // Oldest retained record sits `m_count` slots back from the write head.
    const std::size_t first = (m_head + m_capacity - m_count) % m_capacity;
    out.reserve(m_count);
    for (std::size_t i = 0; i < m_count; ++i) {
        const MatchEvent& e = m_ring[(first + i) % m_capacity];
        if (e.seq > afterSeq)
            out.push_back(e);
    }
    return out;
}

std::vector<MatchEvent> MatchEventLog::tail(std::size_t count) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<MatchEvent> out;
    const std::size_t n = std::min(count, m_count);
    if (n == 0)
        return out;

    const std::size_t first = (m_head + m_capacity - n) % m_capacity;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        out.push_back(m_ring[(first + i) % m_capacity]);
    return out;
}

std::size_t MatchEventLog::size() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_count;
}

uint64_t MatchEventLog::nextSeq() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_nextSeq;
}

uint64_t MatchEventLog::droppedCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_dropped;
}

bool MatchEventLog::hasGapBefore(uint64_t afterSeq) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_count == 0)
        return m_dropped > 0;
    const std::size_t first = (m_head + m_capacity - m_count) % m_capacity;
    const uint64_t oldestSeq = m_ring[first].seq;
    // A consumer resuming at `afterSeq` expects the next record to be afterSeq + 1. If the oldest
    // record we still hold is newer than that, the records in between are gone.
    return oldestSeq > afterSeq + 1;
}

void MatchEventLog::clear() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_head = 0;
    m_count = 0;
    m_dropped = 0;
    // m_nextSeq deliberately NOT reset: seq is a session-monotonic cursor, and rewinding it would
    // make a consumer that resumed at N silently re-receive different events under the same seq.
}

} // namespace fl
