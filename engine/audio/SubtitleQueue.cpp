// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/SubtitleQueue.h"

void SubtitleQueue::push(std::string text, float durationSec) {
    if (!m_enabled || text.empty() || durationSec <= 0.0f)
        return;
    if (m_records.size() >= kMaxActive)
        m_records.pop_front();
    m_records.push_back({std::move(text), durationSec, 0.0f});
}

void SubtitleQueue::update(float dt) {
    for (auto& r : m_records)
        r.elapsedSec += dt;
    while (!m_records.empty() && m_records.front().elapsedSec >= m_records.front().durationSec)
        m_records.pop_front();
}

std::string_view SubtitleQueue::current() const {
    return m_records.empty() ? std::string_view{} : std::string_view{m_records.front().text};
}
