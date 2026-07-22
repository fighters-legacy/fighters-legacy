// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IClock.h"
#include "RenderTypes.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <span>
#include <string_view>

namespace fl {

// The multiplayer kill feed (#647): a small ring of recent "X destroyed Y" lines rendered top-right, the
// HudElement counterpart to the chat/subtitle overlays. Fed from the ClientNetEventHandler CombatEvent
// Kill branch (names resolved via the match roster). Owns persistent char buffers because
// HudElement::text is a non-owning string_view (the ServerNotice pattern) and must outlive endFrame.
//
// Each line lives kLifeSecs (kDwellSecs fully opaque, then kFadeSecs fading out). The clock is injectable
// (IClock) so a test can advance time deterministically. Newest line at the top of the stack.
class KillFeed {
  public:
    static constexpr std::size_t kMaxLines = 6;
    static constexpr float kDwellSecs = 8.f;
    static constexpr float kFadeSecs = 2.f;
    static constexpr float kLifeSecs = kDwellSecs + kFadeSecs;

    void setClock(const fl::IClock& clock) noexcept {
        m_clock = &clock;
    }

    // Push one feed line with an optional RGB tint (default: amber). Overwrites the oldest slot once the
    // ring is full.
    void push(std::string_view line, float r = 1.0f, float g = 0.82f, float b = 0.35f) {
        Entry& e = m_entries[m_head];
        std::snprintf(e.buf, sizeof(e.buf), "%.*s", static_cast<int>(line.size()), line.data());
        e.spawn = m_clock->now();
        e.r = r;
        e.g = g;
        e.b = b;
        e.active = true;
        m_head = (m_head + 1) % kMaxLines;
    }

    // Drop every line (session end / world reset).
    void clear() noexcept {
        for (auto& e : m_entries)
            e.active = false;
        m_head = 0;
    }

    [[nodiscard]] std::span<const HudElement> buildElements() {
        const auto now = m_clock->now();
        std::size_t n = 0;
        // Walk newest-first: the most recent push is at (m_head - 1).
        for (std::size_t i = 0; i < kMaxLines; ++i) {
            const std::size_t idx = (m_head + kMaxLines - 1 - i) % kMaxLines;
            Entry& e = m_entries[idx];
            if (!e.active)
                continue;
            const float age = std::chrono::duration<float>(now - e.spawn).count();
            if (age >= kLifeSecs) {
                e.active = false;
                continue;
            }
            const float alpha = age <= kDwellSecs ? 1.f : std::clamp(1.f - (age - kDwellSecs) / kFadeSecs, 0.f, 1.f);
            HudElement& el = m_elems[n];
            el = {};
            el.type = HudElement::Type::Text;
            el.align = HudAlign::Right;
            el.x = 0.98f;
            el.y = 0.05f + 0.035f * static_cast<float>(n);
            el.scale = 1.f;
            el.r = e.r;
            el.g = e.g;
            el.b = e.b;
            el.a = alpha;
            el.text = e.buf;
            ++n;
        }
        return {m_elems.data(), n};
    }

  private:
    struct Entry {
        char buf[80]{};
        std::chrono::steady_clock::time_point spawn{};
        float r{1.f}, g{1.f}, b{1.f};
        bool active{false};
    };

    const fl::IClock* m_clock{&fl::SystemClock::instance()};
    std::array<Entry, kMaxLines> m_entries{};
    std::array<HudElement, kMaxLines> m_elems{};
    std::size_t m_head{0};
};

} // namespace fl
