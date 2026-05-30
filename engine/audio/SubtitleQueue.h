// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <deque>
#include <string>
#include <string_view>

struct SubtitleRecord {
    std::string text;
    float durationSec;
    float elapsedSec{0.0f};
};

// Per-frame subtitle timing queue. Thread: main only.
class SubtitleQueue {
  public:
    void setEnabled(bool enabled) {
        m_enabled = enabled;
    }
    bool enabled() const {
        return m_enabled;
    }

    // Pushes a subtitle entry. No-op if disabled.
    void push(std::string text, float durationSec);

    // Advances timers; evicts expired entries.
    void update(float dt);

    // Returns the text of the first active record, or an empty view if none.
    std::string_view current() const;

    // All active records (for multi-line subtitle display).
    const std::deque<SubtitleRecord>& records() const {
        return m_records;
    }

  private:
    static constexpr std::size_t kMaxActive = 3; // oldest evicted when exceeded
    bool m_enabled{true};
    std::deque<SubtitleRecord> m_records;
};
